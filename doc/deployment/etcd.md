# The etcd tier

Three instances in an auto scaling group, one per availability zone, that find
each other with `ec2:DescribeInstances` and admit their own replacements to the
cluster. Four resources: a role and an instance profile, a launch template and
the group — and no addresses anywhere in the template.

**The group is the point.** An instance that dies is replaced, and the
replacement removes the member the dead instance was and adds itself in its
place, which is the part an auto scaling group cannot do on its own.

## What discovery has to answer

etcd holds one thing for asyncdb: a key per node,
[`/asyncdb/node/{address}` on a ten second lease](/database/cluster#membership).
Every asyncdb node renews its own every three seconds, and re-registers from
scratch whenever a renewal fails, so an etcd cluster that comes back empty is
fully repopulated within a lease. **There is no state here worth preserving.**

What there is, is an *identity* worth getting right. A member of an etcd cluster
is a name and a peer URL that the other members have agreed on, and the two
questions a booting instance has to answer are:

1. **Is there a cluster already?** If there is, it must join it. If there is not,
   it must bootstrap one — and every node bootstrapping at the same moment must
   name the same membership, or they will form more than one cluster.
2. **Which members are dead?** A member the cluster still counts and no instance
   answers for is a vote that will never be cast.

`DescribeInstances` answers the first half of both: it is the list of instances
that exist, which is the list of members that ought to. The cluster's own
`member list` is the other half — the members it thinks it has. **Discovery here
is the difference between those two lists**, and the boot script is what applies
it.

## Why not a discovery service

The stack used to mint a token from `discovery.etcd.io` in a Lambda-backed
custom resource and pass it to an auto scaling group, and then it did away with
both and fixed three addresses in a `Mappings` block instead. Neither is what is
here now, and both are worth recording:

- **The token service is unmaintained.** It runs on the v2 storage engine that
  v3 replaced, and etcd 3.6 removes v2 discovery outright. A stack whose creation
  depends on a third party's deprecated endpoint answering is a stack that rolls
  back the day it stops.
- **There was no version to upgrade to.** v3 discovery — `--discovery-token`
  with `--discovery-endpoints` — bootstraps from *another* etcd cluster, which is
  no use to the only etcd cluster in the stack.
- **A token is used up once.** `size=3` filled by the first three to arrive, so a
  replacement instance booting with the same token joined nothing. The auto
  scaling group it was supposed to make self-healing could not heal it.
- **Fixed addresses could not heal either.** They made the bootstrap trivial and
  the *replacement* impossible: `update-stack` recreated an instance at the same
  address with an empty data directory and a new member id, and the survivors
  still held the old id for that name. It would not rejoin, and the remedy was a
  hand on `etcdctl`.

`DescribeInstances` is none of those things. It is an AWS API that is already
reachable from a private subnet, it is authoritative about which instances exist
rather than about which ones once registered, and it answers the same for the
third instance of a create and for the replacement of a node that died on a
Tuesday.

## The launch template

`EtcdLaunchTemplate` is `BaseAmi`, `InstanceType`, thirty gigabytes of gp3,
`EtcdSecurityGroup`, `EtcdInstanceProfile`, a `Name=etcd` tag — **which is what
discovery filters on** — and user data. There is no `KeyName`, because
[there is no SSH](/deployment/network#getting-onto-an-instance).

```bash
#! /bin/bash
systemctl enable --now docker
mkdir -p /var/lib/etcd
REGION=eu-west-2
REGISTRY_URL=332187735950.dkr.ecr.eu-west-2.amazonaws.com
IMAGE=$REGISTRY_URL/etcd:v3.5.9    # { "Ref": "EtcdVersion" } for the tag
aws ecr get-login-password --region $REGION | docker login --username AWS --password-stdin $REGISTRY_URL
docker pull $IMAGE
VPC=vpc-0123456789abcdef0          # { "Ref": "VPC" }
SIZE=3
TOKEN=$(curl -s -X PUT http://169.254.169.254/latest/api/token -H "X-aws-ec2-metadata-token-ttl-seconds: 60")
SELF=$(curl -s -H "X-aws-ec2-metadata-token: $TOKEN" http://169.254.169.254/latest/meta-data/local-ipv4)
NAME=etcd-$(curl -s -H "X-aws-ec2-metadata-token: $TOKEN" http://169.254.169.254/latest/meta-data/instance-id)
```

The identity is the **instance id**, and the address is the private one, both
read over [IMDSv2](/deployment/database#the-launch-template). A member is
therefore `etcd-i-0abc…=http://10.0.1.37:2380`, and no two instances that ever
existed can collide on it — which is the whole reason the old `etcd-1`,
`etcd-2`, `etcd-3` could not be reused by a replacement.

### Where the image comes from

`quay.io/coreos/etcd` is where the tag originates and **not** where an instance
gets it. The tier is in
[a private subnet whose only route out is to the named AWS
services](/deployment/network#why-the-instances-are-private), so quay.io is not
reachable from it at all; the tag is
[mirrored into ECR by the build](/pipeline/#mirroring-etcd), and the pull here is
the same login and the same registry the
[database tier](/deployment/database#the-launch-template) uses one endpoint over.
That is why `EtcdRole` carries `AmazonEC2ContainerRegistryReadOnly` alongside its
`DescribeInstances`.

The tag is written by hand in exactly one place, the `etcd-version` file, which
the build mirrors and then writes to `/asyncdb/etcd` for `EtcdVersion` to resolve
at deploy time. `docker-compose.yml` pins the same version against quay.io
directly, because a laptop has an internet connection and no ECR login.

The `docker pull` is **not** redundant with the `docker run` at the end of this
script: `etcdctl()` below runs out of the same image, and it runs first.

### The membership the instances imply

```bash
discover() {
  aws ec2 describe-instances --region $REGION \
    --filters Name=tag:Name,Values=etcd Name=vpc-id,Values=$VPC Name=instance-state-name,Values=pending,running \
    --query 'Reservations[].Instances[].[InstanceId,PrivateIpAddress]' --output text \
    | awk 'NF == 2 { print "etcd-" $1 "=http://" $2 ":2380" }' | sort | paste -sd,
}
```

Three filters and a `--query`, and the shape of the output is deliberate: it is
**exactly the `--initial-cluster` string** etcd wants, built from instances
rather than written down.

| Filter | Keeps out |
| --- | --- |
| `tag:Name=etcd` | The six database instances, which carry `Name=asyncdb` |
| `vpc-id` | Anything with the same tag in another VPC of the same account |
| `instance-state-name=pending,running` | The instance this one is replacing, and every instance the stack ever terminated |

`sort` is what makes it agree between nodes. Every instance runs the same query
and gets the same set in whatever order the API felt like; sorting it means the
three of them bootstrap from **the same string**, which is what stops a
simultaneous start becoming three clusters of one.

```bash
for attempt in $(seq 60); do
  CLUSTER=$(discover)
  [ $(echo $CLUSTER | tr ',' ' ' | wc -w) -ge $SIZE ] && break
  sleep 5
done
```

`SIZE` is the group's desired capacity written a second time, and this loop is
the only reason it has to be. **An instance that bootstraps before its peers are
visible bootstraps a smaller cluster**, so a node waits up to five minutes for
the API to show it three of them. A replacement sees three at once — itself and
the two survivors — so the wait costs a launch nothing; it is the first minute of
a `create-stack` it is there for.

### Bootstrap or join

```bash
MEMBERS=$(etcdctl member list)      # against the other instances' 2379
```

That one call is the decision, and it has three outcomes.

| `member list` | Means | The node |
| --- | --- | --- |
| Nothing answers | No cluster yet, or none with a quorum | Bootstraps: `--initial-cluster $CLUSTER`, state `new` |
| Answers, and already lists this node | This node was named in somebody's bootstrap | Starts as that member: `$CLUSTER`, state `new` |
| Answers, and does not | There is a cluster to be admitted to | Prunes, adds itself, starts with what the add returned, state `existing` |

The middle row is not an edge case. Three instances launch together, one of them
gets there first and bootstraps with all three names in `--initial-cluster`, and
by the time the third one asks there is a two-node cluster that **already counts
it as a member**. Adding itself again would fail, so it does not: it starts with
the same string the others did, and joins the cluster it was already part of.

etcdctl is the image the AMI already carries, run for one command at a time:

```bash
etcdctl() {
  docker run --rm --network host $IMAGE /usr/local/bin/etcdctl --endpoints=$PEERS "$@"
}
```

### The member dance, automated

This is the part that used to be a paragraph of instructions:

```bash
echo "$MEMBERS" | while IFS=, read -r ID STATUS MEMBER PEER REST; do
  echo "$ENTRIES" | grep -qx "$MEMBER=$PEER" || etcdctl member remove $ID
done
ADDED=$(etcdctl member add $NAME --peer-urls=http://$SELF:2380)
INITIAL=$(echo "$ADDED" | grep '^ETCD_INITIAL_CLUSTER=' | cut -d'"' -f2)
```

**A member is pruned when its name and peer URL together are not an instance
that is running.** Matching on both matters: a replacement can be handed the
private address of the instance it replaced, and an address alone would then
recognise a dead member as itself and leave it in the cluster — where it would
collide with the `member add` that follows.

`member add` prints the cluster it just changed, as
`ETCD_INITIAL_CLUSTER="…"`, and that string is what the node starts with. It is
not derived, guessed, or rebuilt from `DescribeInstances`: it is the cluster's
own answer, which is the only thing `--initial-cluster-state existing` will
accept — etcd validates the list against the members it knows and refuses a node
whose count disagrees.

```bash
[ -z "$INITIAL" ] && exit 1
```

**A node that found a cluster and could not join it does not start.** That is the
one deliberate refusal in the script: if `member add` fails — which is what
losing two of three looks like, because a cluster with no quorum cannot agree to
admit anybody — then falling back to a bootstrap would build a *second* cluster
alongside the survivor, and the database tier would hold endpoints for both.
Better an instance with no container, which
[the runbook can see](/runbook/membership#etcd-has-lost-quorum), than a
membership that splits.

### The run

```bash
docker run -d --restart always --name etcd -p 2379:2379 -p 2380:2380 \
  -v /var/lib/etcd:/etcd-data \
  $IMAGE /usr/local/bin/etcd \
  --name $NAME \
  --data-dir /etcd-data \
  --advertise-client-urls http://$SELF:2379 \
  --listen-client-urls http://0.0.0.0:2379 \
  --initial-advertise-peer-urls http://$SELF:2380 \
  --listen-peer-urls http://0.0.0.0:2380 \
  --initial-cluster $INITIAL \
  --initial-cluster-state $STATE \
  --initial-cluster-token asyncdb
```

Four things about it, three of which have not changed:

- **It advertises private addresses.** Peers and clients are told to come in over
  the VPC, which is what lets both ports be closed to everything but the two
  security groups — see [the network](/deployment/network#the-security-groups).
  There is nothing else to advertise: the instances are in
  [private subnets](/deployment/network) and have no public address at all.
- **`/usr/local/bin/etcd` is named explicitly**, as `docker-compose.yml` names
  it: the image has no entrypoint of its own, and the flags are the v3 ones
  (`--name`, not `-name`) on the v3.5.9 that serves the
  [JSON gateway](/database/cluster#membership) asyncdb speaks.
- **`--restart always`** is for the container, and the group is for the instance.
  A container that exits comes back with the data directory it left behind and
  the member it already is; an instance that fails its status checks is replaced,
  and the replacement does the dance above.
- **`--initial-cluster-state` is read once, at bootstrap.** A reboot finds
  `/var/lib/etcd` populated and rejoins as the member it was, whatever the flag
  says — and user data does not run again on a reboot in any case. The
  `--initial-cluster-token` is what keeps this cluster's members from ever
  joining another one by accident.

## The group

```json
"EtcdAutoScalingGroup": {
  "Type": "AWS::AutoScaling::AutoScalingGroup",
  "DependsOn": [ "Ec2Endpoint", "EtcdClientIngress", "EtcdPeerIngress" ],
  "Properties": {
    "VPCZoneIdentifier": [ "…PrivateSubnet1", "…2", "…3" ],
    "LaunchTemplate": { "…": "EtcdLaunchTemplate at LatestVersionNumber" },
    "DesiredCapacity": 3,
    "MinSize": "1",
    "MaxSize": "3",
    "HealthCheckType": "EC2",
    "HealthCheckGracePeriod": 300
  }
}
```

Three subnets and three instances is one per availability zone, arrived at by
counting instances exactly as
[the database tier's six](/deployment/database#the-auto-scaling-group) are. Two
of the numbers are load-bearing:

- **`MaxSize` is 3, not 4.** A group that may exceed its desired capacity
  launches the replacement *before* terminating what it replaces, and a
  replacement that boots while the dying instance is still running sees four
  instances, prunes nothing and adds itself as a fourth member. Four members
  with three alive still has a quorum, so it is not a failure — but it is a
  membership that no longer matches the fleet, and holding the ceiling at the
  desired capacity is what keeps the two the same.
- **`HealthCheckGracePeriod` is 300 seconds**, against the 200 of the database
  tier, because this user data can wait five minutes for its peers to appear
  before it starts anything.

The `DependsOn` names the three things the boot script needs and nothing in a
launch template references: the [EC2 endpoint](/deployment/network#the-endpoints)
`DescribeInstances` goes through, and the two security group rules that let a
launching node reach 2379 and 2380 on the instances already running.

`HealthCheckType` is `EC2` because there is no target group in front of etcd to
ask an application question with. An instance whose etcd never started is one the
group leaves running, which is
[the same hole the database tier has](/runbook/deployment#the-group-does-not-replace-a-failed-application)
and has the same remedy: terminate it and let the group launch another.

## How the database tier finds it

The same query, without the join:

```bash
for attempt in $(seq 12); do
  ASYNCDB_ETCD=$(aws ec2 describe-instances --region eu-west-2 \
    --filters Name=tag:Name,Values=etcd Name=vpc-id,Values=$VPC Name=instance-state-name,Values=running \
    --query 'Reservations[].Instances[].PrivateIpAddress' --output text \
    | tr '\t' '\n' | grep . | sort | sed -e 's|^|http://|' -e 's|$|:2379|' | paste -sd,)
  [ -n "$ASYNCDB_ETCD" ] && break
  sleep 5
done
```

`ASYNCDB_ETCD` takes
[every member separated by commas](/database/cluster#turning-it-on) and stays
with whichever answered, so naming all three *is* the failover — which is why
there is still no load balancer and no DNS in front of etcd. The retry is for the
order the two groups come up in: a database instance that launches before any
etcd instance is running would otherwise start
[owning the whole keyspace](/database/cluster#turning-it-on) on its own.

**It is read once, at boot.** An asyncdb node holds the addresses it was given
until it is itself replaced, so an etcd instance that is replaced leaves every
running database node with one stale endpoint out of three. The client tries them
in turn, so two good ones are enough — but the property to know is that
**replacing all three etcd instances without replacing a database instance
strands the database tier**, and the remedy is to roll the database tier after
it. Nothing in the stack does that automatically.

## What a failure looks like

| What happened | What the cluster does | What heals it |
| --- | --- | --- |
| A container exited | The instance restarts it, and it rejoins as the member it was | `--restart always` |
| One instance died | Two members remain, they have quorum, and writes carry on. The database tier finds one endpoint unreachable and stays with one that answers | The group launches a replacement, which prunes the dead member and adds itself |
| An availability zone went | The same thing: it is one instance | The same thing, once the zone is back — the group cannot launch into a zone that is gone |
| Two instances died | **No quorum.** etcd stops answering writes; asyncdb node registrations stop renewing and the membership drains | Nothing automatic. The replacements find a cluster that cannot admit them, refuse to start, and wait for a hand — see [the runbook](/runbook/membership#etcd-has-lost-quorum) |
| An instance came up with no container | Nothing. `EC2` health checks pass on an instance whose etcd never started | Terminate it, and the group launches another |

The first three are what "self-healing" means here, and they are the common
cases. The fourth is the one that is deliberately left alone, because the
automatic answer to it — bootstrap a fresh cluster — is indistinguishable from a
split brain while any old node is still running.

## What is left to it

- **A dead member is pruned by its replacement, not by a watcher.** Between an
  instance dying and its replacement joining, the cluster counts a member that
  will not vote. Three members tolerate one of those; nothing removes it any
  sooner than the launch does.
- **Losing quorum is still a manual recovery**, and the data being disposable is
  what makes the manual recovery cheap: deleting all three instances and letting
  the group launch three more is a clean bootstrap that costs one lease of stale
  membership.
- **There is no TLS and no authentication.** 2379 is open to the database tier's
  security group and to etcd's own, and 2380 to etcd's own, so anything on the
  database tier can read and write the membership — including writing in a node
  that does not exist, which the other nodes would then forward keys to.
- **Three is the number in two places** — `DesiredCapacity` and the `SIZE` the
  user data waits for — and nothing makes them agree. Growing to five means
  editing both, and it is still a template change rather than a capacity change.
- **`DescribeInstances` is account-wide.** The IAM action cannot be scoped to a
  resource, so both roles can list every instance in the account; the `vpc-id`
  filter is what makes the *answer* this stack's, not the permission.
