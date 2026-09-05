# The database tier

Three instances running the asyncdb image, behind an application load balancer.
Seven resources: a role and an instance profile, a launch template, an auto
scaling group, and the load balancer, its target group and its listener.

## The role

`InstanceRole` is assumable by `ec2.amazonaws.com` and carries two AWS managed
policies:

| Policy | For |
| --- | --- |
| `AmazonEC2ContainerRegistryReadOnly` | `aws ecr get-login-password` and the `docker pull` that follows |
| `service-role/AmazonEC2ContainerServiceforEC2Role` | Nothing here — it is the policy the ECS agent needs, and there is no ECS cluster in this stack |

The second one comes with the ECS-optimised AMI by habit rather than by need,
and it is the wider of the two. Removing it costs nothing.

`InstanceProfile` wraps the role, and `LaunchTemplateData.IamInstanceProfile`
names it by `Ref` — the profile's generated name — which is why the stack needs
`CAPABILITY_NAMED_IAM` even though neither resource sets a name of its own.

## The launch template

`ImageId` and `InstanceType` come from the [parameters](/deployment/#parameters),
the security group is `InstanceSecurityGroup`, the key pair is the `asyncdb` one
that has to exist already, the root volume is [thirty gigabytes of
gp3](#the-root-volume), and the rest is user data — a base64 `Fn::Join` of
shell lines, run once as root at first boot:

```bash
#! /bin/bash
sudo yum -y update
sudo yum -y install unzip
curl "https://awscli.amazonaws.com/awscli-exe-linux-x86_64.zip" -o "awscliv2.zip"
unzip awscliv2.zip
./aws/install --update
REGISTRY_URL=332187735950.dkr.ecr.eu-west-2.amazonaws.com
VERSION=0.0.3          # { "Ref": "Version" }, resolved from SSM at deploy time
IMAGE=$REGISTRY_URL/asyncdb:$VERSION
aws ecr get-login-password --region eu-west-2 | docker login --username AWS --password-stdin $REGISTRY_URL
docker pull $IMAGE
TOKEN=$(curl -s -X PUT http://169.254.169.254/latest/api/token -H "X-aws-ec2-metadata-token-ttl-seconds: 60")
PRIVATE_IP=$(curl -s -H "X-aws-ec2-metadata-token: $TOKEN" http://169.254.169.254/latest/meta-data/local-ipv4)
ZONE=$(curl -s -H "X-aws-ec2-metadata-token: $TOKEN" http://169.254.169.254/latest/meta-data/placement/availability-zone)
ASYNCDB_ETCD=http://10.0.0.10:2379,http://10.0.1.10:2379,http://10.0.2.10:2379
docker run -d -p 80:80 -p 8080:8080 \
  -e ASYNCDB_ETCD=$ASYNCDB_ETCD \
  -e ASYNCDB_NODE=http://$PRIVATE_IP:8080 \
  -e ASYNCDB_ZONE=$ZONE \
  $IMAGE
```

Docker is already there — that is what the AMI is for. The AWS CLI is installed
because `get-login-password` is a v2 command and there is no guarantee of a v2 on
the image: the Amazon Linux 2 the template used to run had v1, and Amazon Linux
2023 ships v2 but not on every variant. `--update` is what makes the install
idempotent over whichever of the two is already there, rather than exiting on the
one it finds.

The private address and the zone are read over **IMDSv2**. Amazon Linux 2023 AMIs are
registered as IMDSv2-only, so the token `PUT` is not belt and braces — the
unauthenticated `GET` the Amazon Linux 2 script used answers `401` on this image,
and `ASYNCDB_NODE` would be `http://:8080`.

The registry account and the region are literals; the version is not. That line
is the template's `Version` parameter — an
`AWS::SSM::Parameter::Value<String>` reading `/asyncdb/version`, which the build
writes after it pushes — joined into the script, so the shell sees a tag and
CloudFormation resolved it at deploy time. The consequence of the two that *are*
literals is on the [overview](/deployment/#before-the-first-deploy): the stack is
really only deployable into one account's `eu-west-2`.

The last five lines are what make the instance a member of a cluster rather than
a database of its own:

- **`ASYNCDB_ETCD`** is `Fn::FindInMap` of the etcd tier's
  [`ClientEndpoints`](/deployment/etcd#the-addresses) — all three, comma
  separated, which is the form that survives one of them being down.
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
  the group spans three subnets in three zones and launches three instances, so
  each zone holds one node and each node holds the whole keyspace. A write goes
  to all three, and a read is answered by whichever node the load balancer
  picked.

Set neither of the first two and the instance would be
[what it was before](/database/cluster#turning-it-on): one process owning the
whole keyspace, talking to nothing.

There is no `docker run --restart`, so a container that stops does not come back
on its own. The instance is replaced for it, because the group's health check is
`ELB` and the container is the only thing that answers the path the load balancer
asks for.

## The root volume

```json
"BlockDeviceMappings": [
  {
    "DeviceName": "/dev/xvda",
    "Ebs": {
      "VolumeSize": 30,
      "VolumeType": "gp3",
      "DeleteOnTermination": true
    }
  }
]
```

There was no `BlockDeviceMappings` here at all, and an instance took whatever the
AMI's snapshot specified: thirty gigabytes of **gp2**. The size was never the
problem — the IO credits were. gp2 earns three IOPS per gigabyte, so a thirty
gigabyte volume has a baseline of a hundred, burstable to three thousand only
while the bucket lasts. RocksDB is an LSM tree: a write is a write-ahead log
append now and a compaction read-and-rewrite later, so a run of sustained writes
spends credits faster than it earns them and then falls to the baseline —
`write_stalled` on a volume that looked fast for the first few minutes of a load
test.

**gp3 has no credits.** Three thousand IOPS and 125 MB/s are the floor, not a
burst, they are the same in the tenth minute as in the first, and at this size
gp3 is slightly cheaper than the gp2 it replaces. Declaring the mapping also
means the number is the template's rather than the AMI's, so a new ECS AMI cannot
change it underneath the stack.

`/dev/xvda` is the root device of the AMI, so this is the volume
the whole instance runs on and not a second one: the OS, docker's image store,
the logs and the database all share it. That is a thing to know rather than a
thing this fixes — [nothing here is
durable](/deployment/#what-this-stack-does-not-do), the container mounts no
volume, and RocksDB is writing into the overlay filesystem at `/tmp/asyncdb`.
gp3 makes that storage predictable. It does not make it persistent, and it is
not the instance-local NVMe a write-heavy store would want.

## The auto scaling group

`AutoScalingGroup` spans all three subnets, launches from the launch template at
`LatestVersionNumber`, registers into `ALBTargetGroup`, and is
`DesiredCapacity: 3` between `MinSize: 1` and `MaxSize: 4`.

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
| `ApplicationLoadBalancer` | Named `ClusterALB`, `internet-facing`, in the three public subnets, in `ALBSecurityGroup` |
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
