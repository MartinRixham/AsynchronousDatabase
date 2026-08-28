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
sudo yum update
sudo yum -y install unzip
curl "https://awscli.amazonaws.com/awscli-exe-linux-x86_64.zip" -o "awscliv2.zip"
unzip awscliv2.zip
./aws/install
REGISTRY_URL=332187735950.dkr.ecr.eu-west-2.amazonaws.com
VERSION=0.0.2
IMAGE=$REGISTRY_URL/asyncdb:$VERSION
aws ecr get-login-password --region eu-west-2 | docker login --username AWS --password-stdin $REGISTRY_URL
docker pull $IMAGE
docker run -d -p 80:80 $IMAGE
```

Docker is already there — that is what the AMI is for. The AWS CLI is installed
because the version on Amazon Linux 2 is v1, and `get-login-password` is a v2
command; installing v2 over it is the shortest way to a working `docker login`.
Note that `sudo yum update` has no `-y` and nothing is reading its prompt, so
that line does nothing but slow the boot down; the install below it does have
one.

The registry account and the region are literals, and so is the version. The
consequences are on the [overview](/deployment/#before-the-first-deploy): the
stack is really only deployable into one account's `eu-west-2`, and a new
release is a change to this template.

**No environment is passed to the container.** `ASYNCDB_ETCD` and
`ASYNCDB_NODE` are what turn an instance into a member of a cluster, and without
them the process owns the whole keyspace and talks to nothing. Setting them here
means reading the instance's own address out of the metadata service, the way
[the etcd tier](/deployment/etcd#the-launch-template) already does, and naming
the etcd nodes — which is the harder half, because their addresses are not known
to this template. A load-balanced address for the etcd tier, or a fixed set of
private addresses, is what that wants.

There is no `docker run --restart`, so a container that stops does not come back
until the instance is replaced, and the instance is not replaced for it because
the group's health check is `EC2`.

## The auto scaling group

`AutoScalingGroup` spans all three subnets, launches from the launch template at
`LatestVersionNumber`, registers into `ALBTargetGroup`, and is
`DesiredCapacity: 3` between `MinSize: 1` and `MaxSize: 4`.

Three, spread over three subnets, is one instance per availability zone. The
bounds allow a fourth for a replacement to come up before an old one goes, and
allow the group to be taken down to one by hand, but nothing moves it on its
own: there is no scaling policy and no alarm in the template.

The health check type is left at its default, `EC2`, so the group replaces an
instance whose *instance* has failed and not one whose *application* has. An
instance whose pull failed, or whose container exited, stays in the group; the
load balancer stops sending it traffic and the group never notices.
`HealthCheckType: ELB` with a grace period long enough for the user data to
finish is the fix, and the grace period matters — the user data installs the
AWS CLI before it pulls, and an instance failing its health check before it has
had time to start is a group that replaces instances forever.

There is also no `UpdatePolicy`, so
[a new version is not rolled out](/deployment/#rolling-out-a-new-version) by
`update-stack` alone.

## The load balancer

| Resource | Is |
| --- | --- |
| `ApplicationLoadBalancer` | Named `ClusterALB`, `internet-facing`, in the three public subnets, in `ALBSecurityGroup` |
| `ALBTargetGroup` | HTTP, port 80, `TargetType: instance`, health check `GET /` |
| `ALBListener` | HTTP on port 80, one default action forwarding to the target group |

Port 80 on an instance is nginx, so the target group is the UI and the
`/asyncdb/*` proxy in front of the API — the same thing a browser sees at
`localhost:8080` under `docker-compose`. The API port is not a target, and
should not be: it is the port nodes talk to each other on, and it has no
authentication of its own.

The name is fixed, so there can be one of these per region, and there are no
outputs, so its DNS name is found afterwards rather than reported:

```bash
aws elbv2 describe-load-balancers --names ClusterALB \
  --query 'LoadBalancers[0].DNSName' --output text
```

The health check is `GET /`, which is nginx serving `index.html` out of
`/usr/share/nginx/html`. That answers as soon as nginx is up, whether or not the
database behind it is: nginx and the binary are started together by the image's
`CMD nginx & ./asyncdb`, and nothing in the image makes one wait for the other.
`GET /asyncdb/health` is the check that means what it says — it is
[the endpoint that names the cluster](/database/cluster#what-each-endpoint-does-in-a-cluster)
and it goes through the proxy to the process itself, so it fails while the
database is not answering.

Sessions are not sticky, and with the instances not clustered that is the whole
of the problem the [overview](/deployment/#what-this-stack-does-not-do)
describes: consecutive requests from one client land on different databases.
Stickiness would hide it and not fix it — the fix is the two environment
variables.
