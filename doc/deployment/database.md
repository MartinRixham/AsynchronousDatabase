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
that has to exist already, and the rest is user data — a base64 `Fn::Join` of
shell lines, run once as root at first boot:

```bash
#! /bin/bash
sudo yum -y update
sudo yum -y install unzip
curl "https://awscli.amazonaws.com/awscli-exe-linux-x86_64.zip" -o "awscliv2.zip"
unzip awscliv2.zip
./aws/install
REGISTRY_URL=332187735950.dkr.ecr.eu-west-2.amazonaws.com
VERSION=0.0.3          # { "Ref": "Version" }, resolved from SSM at deploy time
IMAGE=$REGISTRY_URL/asyncdb:$VERSION
aws ecr get-login-password --region eu-west-2 | docker login --username AWS --password-stdin $REGISTRY_URL
docker pull $IMAGE
PRIVATE_IP=$(curl -s http://169.254.169.254/latest/meta-data/local-ipv4)
ASYNCDB_ETCD=http://10.0.0.10:2379,http://10.0.1.10:2379,http://10.0.2.10:2379
docker run -d -p 80:80 -p 8080:8080 \
  -e ASYNCDB_ETCD=$ASYNCDB_ETCD \
  -e ASYNCDB_NODE=http://$PRIVATE_IP:8080 \
  $IMAGE
```

Docker is already there — that is what the AMI is for. The AWS CLI is installed
because the version on Amazon Linux 2 is v1, and `get-login-password` is a v2
command; installing v2 over it is the shortest way to a working `docker login`.

The registry account and the region are literals; the version is not. That line
is the template's `Version` parameter — an
`AWS::SSM::Parameter::Value<String>` reading `/asyncdb/version`, which the build
writes after it pushes — joined into the script, so the shell sees a tag and
CloudFormation resolved it at deploy time. The consequence of the two that *are*
literals is on the [overview](/deployment/#before-the-first-deploy): the stack is
really only deployable into one account's `eu-west-2`.

The last four lines are what make the instance a member of a cluster rather than
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

Set neither and the instance would be
[what it was before](/database/cluster#turning-it-on): one process owning the
whole keyspace, talking to nothing.

There is no `docker run --restart`, so a container that stops does not come back
on its own. The instance is replaced for it, because the group's health check is
`ELB` and the container is the only thing that answers the path the load balancer
asks for.

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

`HealthCheckGracePeriod` is `600` seconds, and the number matters. The user data
installs the AWS CLI before it logs in and pulls, and an instance marked unhealthy
before it has had time to start is a group that replaces instances forever. Ten
minutes is comfortably more than a cold pull takes and is measured from the
launch, not from the first check.

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
