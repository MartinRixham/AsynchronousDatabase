# The database tier

Six instances running the asyncdb image, two in each availability zone, behind
an application load balancer.
Seven resources: a role and an instance profile, a launch template, an auto
scaling group, and the load balancer, its target group and its listener.

## The role

`InstanceRole` is assumable by `ec2.amazonaws.com` and carries two AWS managed
policies and one inline policy of its own:

| Policy | For |
| --- | --- |
| `AmazonEC2ContainerRegistryReadOnly` | `aws ecr get-login-password` and the `docker pull` that follows |
| `AmazonSSMManagedInstanceCore` | Session Manager, which is [the only way onto an instance](/deployment/network#getting-onto-an-instance) |
| `discovery`, inline | `ec2:DescribeInstances`, which is how the user data below [finds the etcd tier](/deployment/etcd#how-the-database-tier-finds-it) |

`service-role/AmazonEC2ContainerServiceforEC2Role`, the policy the ECS agent
needs, is deliberately **not** among them. The instances launch from the
ECS-optimised image because of
[the Docker daemon on it and not for ECS](/deployment/#parameters), and there is
no cluster in this stack for an agent to register with.

The inline one is the tier's own, and it is the same policy
[the etcd tier carries](/deployment/etcd). `ec2:DescribeInstances` takes no
resource, so `"Resource": "*"` is the only form it has — the `vpc-id` filter in
the query is what narrows the answer, not the grant.

`InstanceProfile` wraps the role, and `LaunchTemplateData.IamInstanceProfile`
names it by `Ref` — the profile's generated name — which is why the stack needs
`CAPABILITY_NAMED_IAM` even though neither resource sets a name of its own.

## The launch template

`ImageId` and `InstanceType` come from the [parameters](/deployment/#parameters),
the security group is `InstanceSecurityGroup`, there is no key pair because
[there is no SSH](/deployment/network#getting-onto-an-instance), the root volume
is [thirty gigabytes of gp3](#the-root-volume), `TagSpecifications` names every instance the group
launches `asyncdb` — which is what
[`Name=tag:Name,Values=asyncdb`](/runbook/deployment) finds, and what tells the
six of them from the three `etcd-n` in the console — and the rest is user data — a base64 `Fn::Sub`
of a shell script, run once as root at first boot:

```bash
#! /bin/bash
systemctl enable --now docker
mkdir -p /var/lib/asyncdb
REGION=eu-west-2
# The subnet has no IPv4 route out, so every call to AWS goes over IPv6 to a dual stack endpoint.
export AWS_USE_DUALSTACK_ENDPOINT=true
[ -f /etc/amazon/ssm/amazon-ssm-agent.json ] || cp /etc/amazon/ssm/amazon-ssm-agent.json.template /etc/amazon/ssm/amazon-ssm-agent.json
python3 -c "import json; f = '/etc/amazon/ssm/amazon-ssm-agent.json'; c = json.load(open(f)); c.setdefault('Agent', {}).update({'Region': '$REGION', 'UseDualStackEndpoint': True}); json.dump(c, open(f, 'w'), indent = 2)"
systemctl restart amazon-ssm-agent
REGISTRY_URL=332187735950.dkr-ecr.eu-west-2.on.aws
VERSION=0.0.3          # ${Version}, resolved from SSM at deploy time
IMAGE=$REGISTRY_URL/asyncdb:$VERSION
aws ecr get-login-password --region $REGION | docker login --username AWS --password-stdin $REGISTRY_URL
docker pull $IMAGE
TOKEN=$(curl -s -X PUT http://169.254.169.254/latest/api/token -H "X-aws-ec2-metadata-token-ttl-seconds: 60")
PRIVATE_IP=$(curl -s -H "X-aws-ec2-metadata-token: $TOKEN" http://169.254.169.254/latest/meta-data/local-ipv4)
ZONE=$(curl -s -H "X-aws-ec2-metadata-token: $TOKEN" http://169.254.169.254/latest/meta-data/placement/availability-zone)
VPC=vpc-0123456789abcdef0                  # ${VPC}
for attempt in $(seq 12); do
  ASYNCDB_ETCD=$(aws ec2 describe-instances --region $REGION \
    --filters Name=tag:Name,Values=etcd Name=vpc-id,Values=$VPC Name=instance-state-name,Values=running \
    --query 'Reservations[].Instances[].PrivateIpAddress' --output text \
    | tr '\t' '\n' | grep . | sort | sed -e 's|^|http://|' -e 's|$|:2379|' | paste -sd,)
  [ -n "$ASYNCDB_ETCD" ] && break
  sleep 5
done
docker run -d --restart always -p 80:80 -p 8080:8080 \
  -v /var/lib/asyncdb:/var/lib/asyncdb \
  -e ASYNCDB_ETCD=$ASYNCDB_ETCD \
  -e ASYNCDB_NODE=http://$PRIVATE_IP:8080 \
  -e ASYNCDB_ZONE=$ZONE \
  $IMAGE
```

The store is bound from the host rather than left in the container's own
filesystem, and `--restart always` is what makes that worth doing: a container
that is restarted opens what the one before it wrote instead of coming back as an
empty node. It is still the [root volume](#the-root-volume), so it goes when the
instance does.

**Docker and the AWS CLI are both already on the image.** They come with
[`BaseAmi`](/deployment/#parameters) — the ECS-optimised Amazon Linux 2023, taken
for the daemon rather than for ECS — so the first two lines are an `enable` that
is very nearly a no-op and a directory for the bind mount below, what follows is
[the dual-stack preparation](/deployment/network#the-route-out) a private subnet
with no IPv4 route out needs, and the rest is a login, a pull and a run.

That is the whole of the host preparation. Nothing updates packages and nothing
installs the AWS CLI: Amazon Linux 2023 ships v2, which is what
`get-login-password` needs, and **a patch level baked in March is staler in June
than the base image AWS republished in May**, so patching belongs to the base
image rather than to boot. Every second spent here is spent on every instance at
every launch, inside a `HealthCheckGracePeriod` of 200 seconds.

The image is **not** on the AMI, and never was. It moves every release, so
baking it would put an image build on the release path; at a few tens of
megabytes from a registry in the same region the pull it would save is seconds.
`Version` resolves `/asyncdb/version` at deploy time, so a release reaches an
instance when that instance is replaced.

The private address and the zone are read over **IMDSv2**. Amazon Linux 2023
AMIs are registered as IMDSv2-only, so the token `PUT` is not belt and braces — the
unauthenticated `GET` the Amazon Linux 2 script used answers `401` on this image,
and `ASYNCDB_NODE` would be `http://:8080`.

The registry account and the region are literals; the version is not. That line
is the template's `Version` parameter — an
`AWS::SSM::Parameter::Value<String>` reading `/asyncdb/version`, which the build
writes after it pushes — substituted into the script by `Fn::Sub`, so the shell
sees a tag and CloudFormation resolved it at deploy time. The consequence of the
two that *are* literals is on the
[overview](/deployment/#before-the-first-deploy): the stack is really only
deployable into one account's `eu-west-2`.

The last five lines are what make the instance a member of a cluster rather than
a database of its own:

- **`ASYNCDB_ETCD`** is every running etcd instance's private address on 2379,
  comma separated, which is the form that survives one of them being down. It
  comma separated, which is the form that survives one of them being down. The
  etcd tier is [a group that discovers itself](/deployment/etcd), so there are no
  addresses to write down and this tier asks the same question the etcd tier
  asks — `DescribeInstances`, filtered to `Name=etcd` in this VPC. The retry is
  for the order the two groups come up in, and **the answer is read once**: an
  etcd instance replaced later leaves this node with one stale endpoint of three,
  which the client passes over, and
  [replacing all three without rolling this tier strands it](/deployment/etcd#how-the-database-tier-finds-it).
- **`ASYNCDB_NODE`** is this instance's own private address on 8080, read from
  the metadata service at boot. It is the **API port**, not the nginx in front
  of it: nodes talk to each other directly and do not go through the `/asyncdb`
  prefix a browser uses, which is why `-p 8080:8080` is published alongside 80
  and why [8080 is open](/deployment/network#the-api-port-is-not-the-load-balancers)
  within the security group.
- **`ASYNCDB_ZONE`** is the instance's own availability zone, `eu-west-2a` and
  not the subnet, read from the same metadata service. It is what makes the
  cluster keep
  [one copy of every record in every zone](/database/cluster#one-copy-in-every-zone):
  the group spans three subnets in three zones and launches six instances, so
  each zone holds two nodes, the two of them split that zone's copy between
  them, and each node holds half the keyspace. A write goes to one node in each
  zone; a read is answered by the node the load balancer picked, or by the one
  other node in that node's own zone, which is traffic that
  [crosses no zone and costs nothing](/deployment/cost#bytes-across-an-availability-zone).

Set neither of the first two and the instance would be
[what it was before](/database/cluster#turning-it-on): one process owning the
whole keyspace, talking to nothing.

There is no `docker run --restart`, so a container that stops does not come back
on its own. The instance is replaced for it, because the group's health check is
`ELB` and the container is the only thing that answers the path the load balancer
asks for.

## The root volume

```yaml
BlockDeviceMappings:
  - DeviceName: /dev/xvda
    Ebs:
      VolumeSize: 30
      VolumeType: gp3
      DeleteOnTermination: true
```

The mapping is declared rather than left to the AMI's snapshot, which specifies
thirty gigabytes of **gp2**. The size is not the problem — the IO credits are.
gp2 earns three IOPS per gigabyte, so a thirty gigabyte volume has a baseline of
a hundred, burstable to three thousand only while the bucket lasts. RocksDB is an
LSM tree: a write is a write-ahead log append now and a compaction
read-and-rewrite later, so a run of sustained writes spends credits faster than
it earns them and then falls to the baseline — `write_stalled` on a volume that
looked fast for the first few minutes of a load test.

**gp3 has no credits.** Three thousand IOPS and 125 MB/s are the floor, not a
burst, they are the same in the tenth minute as in the first, and at this size
gp3 is slightly cheaper than gp2. Declaring the mapping also means the number is
the template's rather than the AMI's, so a new ECS AMI cannot change it
underneath the stack.
change it underneath the stack.

`/dev/xvda` is the root device of the AMI, so this is the volume
the whole instance runs on and not a second one: the OS, docker's image store,
the logs and the database all share it. That is a thing to know rather than a
thing this fixes — [nothing here is
durable](/deployment/#what-this-stack-does-not-do), and RocksDB is writing to
`/var/lib/asyncdb`, which the container binds from the host rather than keeping
in an overlay filesystem that goes when the container does. That is what a
restarted container reopens; it is still the root volume, so it goes when the
instance does. gp3 makes that storage predictable. It does not make it
persistent, and it is not the instance-local NVMe a write-heavy store would want.

## The auto scaling group

`AutoScalingGroup` spans all three **private** subnets, launches from the launch
template at `LatestVersionNumber`, registers into `ALBTargetGroup`, and is
`DesiredCapacity: 6` between `MinSize: 1` and `MaxSize: 7`. It carries a
`DependsOn` naming `PrivateRoute`, because
[the pull and the etcd discovery both go out over it](/deployment/network#the-route-out)
and nothing in the launch template says so.

Six is three zones of two, and the group is what makes it so: an auto scaling
group balances its capacity across the subnets it is given, so six instances
over three subnets is two in each. That is the whole of the arrangement —
[three copies of the keyspace, each split in half](/database/cluster#one-copy-in-every-zone)
— and it is arrived at by counting instances rather than by configuring
anything. `MaxSize` is one above the desired capacity so that a replacement can
launch before the instance it replaces goes; **capacity is worth moving three at
a time**, one per zone, so that no zone is left holding a larger share than the
others.

Three, spread over three subnets, is one instance per availability zone. The
bounds allow a fourth for a replacement to come up before an old one goes, and
allow the group to be taken down to one by hand, but nothing moves it on its
own: there is no scaling policy and no alarm in the template.

`HealthCheckType` is `ELB` rather than the default `EC2`, so the group replaces
an instance whose *application* has failed and not only one whose *instance* has:
an instance whose pull failed, or whose container exited, is one the load balancer
has already stopped sending traffic to, and the group now takes it out too.

`HealthCheckGracePeriod` is `200` seconds, and the number matters. It is measured
from the launch and not from the first check, and everything in the user data has
to fit inside it: a `yum -y update`, a download and install of AWS CLI v2, a
`docker login` and then a cold `docker pull` of the image. An instance marked
unhealthy before it has finished that is replaced by another that starts the same
work from the beginning, which is a group that replaces instances forever.

**Three and a bit minutes is not a generous margin for that sequence.** It holds
because the pull is from ECR in the same region, but a slow `yum` mirror or a
larger image is enough to eat it, and the failure it produces looks like an
instance that never comes up rather than like a timeout. There is room to raise
it: nothing waits on the grace period except the first health check of a genuinely
dead instance.

There is also no `UpdatePolicy`, so
[a new version is not rolled out](/deployment/#rolling-out-a-new-version) by
`update-stack` alone.

## The load balancer

| Resource | Is |
| --- | --- |
| `ApplicationLoadBalancer` | Named `ClusterALB`, `internet-facing`, in the three public subnets — [the only thing in them](/deployment/network) — in `ALBSecurityGroup` |
| `ALBTargetGroup` | HTTP, port 80, `TargetType: instance`, health check `GET /asyncdb/health` |
| `ALBListener` | HTTP on port 80, one default action forwarding to the target group |

Port 80 on an instance is nginx, so the target group is the UI and the
`/asyncdb/*` proxy in front of the API — the same thing a browser sees at
`localhost:8080` under `docker-compose`. The API port is not a target, and
should not be: it is the port nodes talk to each other on, and it has no
authentication of its own.

The name is fixed, so there can be one of these per region. Its DNS name is what
the stack's [`Url` output](/deployment/#the-address) is built from —
`Fn::GetAtt` of `DNSName` with `http://` in front — so the public address of the
deployment comes back from `describe-stacks` and does not have to be looked up:

```bash
aws cloudformation describe-stacks --stack-name asyncdb \
  --query 'Stacks[0].Outputs[?OutputKey==`Url`].OutputValue' --output text
```

The health check is `GET /asyncdb/health`, which goes through the proxy to the
process itself — it is
[the endpoint that names the cluster](/database/cluster#what-each-endpoint-does-in-a-cluster)
— so it fails while the database is not answering. `GET /` would not: that is
nginx serving `index.html` out of `/usr/share/nginx/html`, and it answers as soon
as nginx is up whether or not there is a database behind it. The two are started
together by the image's `CMD nginx & ./asyncdb` and nothing makes one wait for the
other, so checking the static file would be checking the wrong process.

Sessions are not sticky, and do not need to be: every node
[answers for every key](/database/cluster), asking the owner when it is not the
owner, so it does not matter which instance the load balancer picks. The one
exception is
[paging through a scan](/database/scans#paging-and-what-a-cursor-promises),
whose cursor belongs to the instance that issued it — a client paging through
the load balancer should use `from` and `to`, which any node will take.
