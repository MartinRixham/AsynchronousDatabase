# The database tier

Six instances running the asyncdb image, two in each availability zone, behind
an application load balancer.
Seven resources: a role and an instance profile, a launch template, an auto
scaling group, and the load balancer, its target group and its listener.

## The role

`InstanceRole` is assumable by `ec2.amazonaws.com` and carries two AWS managed
policies and two inline policies of its own:

| Policy | For |
| --- | --- |
| `AmazonEC2ContainerRegistryReadOnly` | `aws ecr get-login-password` and the `docker pull` that follows |
| `AmazonSSMManagedInstanceCore` | Session Manager, which is [the only way onto an instance](/deployment/network#getting-onto-an-instance) |
| `discovery`, inline | `ec2:DescribeInstances`, which is how the user data below [finds the etcd tier](/deployment/etcd#how-the-database-tier-finds-it) |
| `logs`, inline | `logs:CreateLogStream` and `logs:PutLogEvents` on the group `asyncdb` alone, which is [where the container logs](#the-logs) |

`service-role/AmazonEC2ContainerServiceforEC2Role`, the policy the ECS agent
needs, is deliberately **not** among them. The instances launch from the
ECS-optimised image because of
[the Docker daemon on it and not for ECS](/deployment/#parameters), and there is
no cluster in this stack for an agent to register with.

The inline ones are the tier's own, and they are the same two policies
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
of a shell script, run once as root at first boot, which is `LaunchTemplate` in
`cloudformation.yaml`. It prepares the host, logs in to ECR, pulls the image,
reads the instance's address and zone, finds the etcd tier and runs the container
with the host's `/var/lib/asyncdb` bound into it.

The store is bound from the host rather than left in the container's own
filesystem, and `--restart always` is what makes that worth doing: a container
that is restarted opens what the one before it wrote instead of coming back as an
empty node. It is still the [root volume](#the-root-volume), so it goes when the
instance does.

**Docker and the AWS CLI are both already on the image.** They come with
[`BaseAmi`](/deployment/#parameters) — the ECS-optimised Amazon Linux 2023, taken
for the daemon rather than for ECS — so the script opens with an `enable` that
is very nearly a no-op and a directory for the bind mount, what follows is
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

The registry account and the region are literals; the version is not. The tag
is the template's `Version` parameter — an
`AWS::SSM::Parameter::Value<String>` reading `/asyncdb/version`, which the build
writes after it pushes — substituted into the script by `Fn::Sub`, so the shell
sees a tag and CloudFormation resolved it at deploy time. The consequence of the
two that *are* literals is on the
[overview](/deployment/#before-the-first-deploy): the stack is really only
deployable into one account's `eu-west-2`.

Three environment variables on the `docker run` are what make the instance a
member of a cluster rather than a database of its own:

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

The container runs `--restart always`, so one that exits comes back by itself
and reopens what the last one wrote: the store is the host's `/var/lib/asyncdb`
and not the container's. What that does not cover is an instance that never got a
container at all: the group's
[health check is `EC2`](#the-auto-scaling-group), and it leaves that one running.

### The logs

The container logs to CloudWatch through Docker's own `awslogs` driver, into the
group `asyncdb`, one stream an instance named `{stack}/asyncdb-{instance id}` —
the stack name is `${AWS::StackName}` in the template. **The logs outlive the
instance**, and that is the point of them: the root volume goes when an instance
is terminated, and the instance a failure is about — the node a chaos experiment
stopped, the one the group replaced — is the one nobody can run `docker logs` on
afterwards.

- **The group is not in this stack.** A group the template made would be deleted
  by `make delete-stack`, and the pipeline deletes a stack as soon as its
  experiments finish. [The build creates it](/pipeline/#keeping-the-logs) and
  keeps seven days of it, so every stack of every run writes into one group and
  a stream goes a week after it was written, whatever became of its stack.
- **`awslogs-endpoint` is the dual-stack name.** The driver runs in the Docker
  daemon on the host, which [has no IPv4 route out](/deployment/network#the-route-out)
  and does not read `AWS_USE_DUALSTACK_ENDPOINT`, and `logs.eu-west-2.amazonaws.com`
  answers on IPv4 alone.
- **`mode=non-blocking`** buffers lines rather than making the process wait on
  CloudWatch for its own stdout, so a logging outage costs lines and never stalls
  the database.
- **`docker logs` still answers**: Docker keeps a local copy beside a remote
  driver, so the [Session Manager commands](/runbook/deployment) and the
  pipeline's diagnosis read what they always did.

A node that is gone is read by its stream:

```bash
aws logs tail asyncdb --log-stream-name-prefix asyncdb-three/asyncdb-i-0123456789abcdef0 --since 1d
```

## The root volume

`BlockDeviceMappings` gives `/dev/xvda` thirty gigabytes of gp3, deleted on
termination. The mapping is declared rather than left to the AMI's snapshot, which specifies
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

`AutoScalingGroup` spans the **private** subnets, launches from the launch
template at `LatestVersionNumber`, registers into `ALBTargetGroup`, and takes its
capacity from `Nodes`, which is 6. It carries a `DependsOn` naming
`PrivateRoute`, because
[the pull and the etcd discovery both go out over it](/deployment/network#the-route-out)
and nothing in the launch template says so.

Six is three zones of two, and the group is what makes it so: an auto scaling
group balances its capacity across the subnets it is given, so six instances
over three subnets is two in each. That is the whole of the arrangement —
[three copies of the keyspace, each split in half](/database/cluster#one-copy-in-every-zone)
— and it is arrived at by counting instances rather than by configuring
anything.

## The two parameters that are the shape

The arrangement has exactly two dimensions, and each of them is one parameter:

| | Is | Values |
| --- | --- | --- |
| `Zones` | How many availability zones the group is given, which is **how many copies of the keyspace there are** — a zone holds exactly one | 2 or 3, default 3 |
| `Nodes` | How many instances the group runs, which is **how many ways a zone splits the copy it holds** | 3, 6 or 9, default 6 |

Neither is read by anything running on an instance. `Zones` is the number of
subnets in `VPCZoneIdentifier` and nothing else; a node reads its own
availability zone [out of IMDS](#the-user-data), so where the group put it is
what it is. `Nodes` is `DesiredCapacity`, and the split inside a zone is arrived
at by counting the instances that registered.

`MaxSize` comes from the `Capacity` mapping rather than from the parameter,
because CloudFormation cannot add one to a number: it is one above the desired
capacity for every allowed value, so that a replacement can launch before the
instance it replaces goes. The allowed values are multiples of three for the same
reason **capacity is worth moving three at a time**, one per zone, so that no zone
is left holding a larger share than the others — all but four, which is there for
`chaos/nodes-removed`: a shrink from six that leaves one zone holding both of its
nodes, and so a copy of every key. Three, spread over three subnets,
is one instance per availability zone; `MinSize: 1` allows the group to be taken
down to one by hand.

Nothing moves either parameter on its own — there is no scaling policy and no
alarm in the template — and moving one is a stack update. An instance that joins
[fills itself in before it registers](/runbook/rebuild), and every node then
[moves the records whose owner moved](/runbook/rebuild#when-ownership-moves) with
the membership — fetching what it has been handed, giving up what has been taken
from it. What no mechanism recovers is a key whose owner in *every* zone was
terminated by the same update, which is why **capacity still moves three at a
time and at a quiet moment**.

**Taking a zone away is the update and then emptying it.** Lowering `Zones` says
which subnets the group may use and nothing about the instances already in the one
it lost: the group rebalances out of that subnet in its own time, which is a
quarter of an hour of a cluster carrying a copy nobody wants. Stop those instances
once the update lands and the group has an unhealthy instance instead of an
unbalanced one — the same
[EC2 health check](/runbook/deployment#the-group-does-not-replace-a-failed-application)
that replaces any other, launching each replacement in a subnet it still spans.
The zone is empty in a couple of minutes rather than fifteen, and the instances
that were holding its copy go with it either way.

`chaos/nodes-added`, `chaos/nodes-removed` and `chaos/zone-retired` move one of
these parameters each and then ask every node what is in its store, asserting
what a resize has to leave behind: every zone holding the same keys, and no key
held by two nodes of one zone. `zone-retired` empties the retired zone the way
this section describes, so what the pipeline runs is the procedure and not a
faster stand-in for it.

`HealthCheckType` is `EC2`, the default, so the group replaces an instance whose
*instance* has failed — a failed EC2 status check — and **not** one whose
*application* has. An instance whose pull failed has no container, and the load
balancer stops sending it traffic while the group leaves it running.
[The runbook](/runbook/deployment#the-group-does-not-replace-a-failed-application)
is what that costs and what to do about it: terminate the instance yourself, and
the group launches another. The etcd tier
[has the same hole for the same reason](/deployment/etcd#the-group).

`HealthCheckGracePeriod` is `200` seconds, and the number matters. It is measured
from the launch and not from the first check, and everything in the user data has
to fit inside it: a `yum -y update`, a download and install of AWS CLI v2, a
`docker login` and then a cold `docker pull` of the image. An instance whose EC2
status checks are evaluated before it has finished is one the group replaces with
another that starts the same work from the beginning.

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
| `ApplicationLoadBalancer` | Named `${AWS::StackName}-alb`, `internet-facing`, in the three public subnets — [the only thing in them](/deployment/network) — in `ALBSecurityGroup`. The name is the stack's because a load balancer name is unique to a region, and [the pipeline stands up four stacks at once](/pipeline/#the-shares). Fifteen second idle timeout |
| `ALBTargetGroup` | HTTP, port 80, `TargetType: instance`, health check `GET /asyncdb/health` every five seconds with a four second timeout, two checks either way, thirty second deregistration delay |
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

**It also fails on a node that is answering.** A node etcd has stopped answering,
or one in a membership too small to claim a leader, refuses every write, and
after a lease it says so here with a `503` — [`unled`](/runbook/#health). That is
the only way a load balancer can be told, and without it an isolated node is
chosen for as long as the fault lasts: up, answering and refusing every write. **Five seconds and two checks** is how long that lasts, and it is the
least the load balancer allows — the defaults are thirty seconds and five checks
to come back, which is two and a half minutes of a node that is whole again
being left out. The group's health check is
[`EC2`](/runbook/deployment#the-group-does-not-replace-a-failed-application), so
nothing replaces the instance over it; it is taken out of service and put back
when it answers `200` again.

A target group with **no** healthy target left in it is one the load balancer
sends to all of them, so etcd lost altogether — where every node is in that
state at once — is a cluster that goes on serving reads rather than one nothing
can reach.

**And it fails on a node that is being stopped, before it stops answering.** An
instance stopped or terminated shuts down cleanly, and Docker stops the container
with a `SIGTERM`. The container is run with `ASYNCDB_DRAIN=15`: for fifteen
seconds the node answers this check `503` with
[`draining`](/runbook/#health) set and serves everything else as normal, and only
then leaves the cluster and closes. Fifteen is two failed checks five seconds
apart and one more; `--stop-timeout 30` is what stops Docker killing the drain
part way through. Without it the load balancer learns a node has gone from the
checks it fails *after* it has gone, and every request sent to it in between is a
`502` — or, once the host is off, a `504` the idle timeout later.

**The idle timeout is fifteen seconds, and not the sixty it defaults to.** The
health check stops *new* requests going to a target that has gone, and does
nothing for one already sent to it. A stopped instance or a zone cut off by the
network sends no reset, so a request on a connection the load balancer was
already holding to it hears nothing at all, and the load balancer answers `504`
only once the connection has been idle this long. Sixty seconds of silence is a
client that gave up without an answer; fifteen is an answer it can retry on.

It is not shorter because a node goes quiet on a request while it is working:
a read or a write that finds a copy gone waits
[`unacknowledged_timeout_seconds`](/runbook/nodes#a-node-that-is-up-but-wrong),
five, before it asks the next copy or refuses, and a read can pass over two.
The timeout counts silence and not the length of a request, so a 16 MiB body
that is still moving is not cut off by it.

**Deregistration is thirty seconds, and not the five minutes it defaults to.** A
draining target is a running instance: it goes on renewing its etcd lease
throughout, so it is still in the membership and still a copy every write waits
for, long after the load balancer has stopped sending it anything. The default is
five minutes in which the cluster is a node larger than the group is, and a
shrink and a replacement both spend it. Thirty seconds is longer than any request
this API answers, [a 16 MiB body included](/database/reference).

Sessions are not sticky, and do not need to be: every node
[answers for every key](/database/cluster), asking the owner when it is not the
owner, so it does not matter which instance the load balancer picks. The one
exception is
[paging through a scan](/database/scans#paging-and-what-a-cursor-promises),
whose cursor belongs to the instance that issued it — a client paging through
the load balancer should use `from` and `to`, which any node will take.
