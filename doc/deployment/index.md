# Deploying to AWS

`cloudformation.yaml` in the root of the repository is the whole of the
infrastructure: one template, one stack, no modules and no state file. It builds
a VPC across three availability zones, an application load balancer, an auto
scaling group of six instances running the asyncdb image from ECR — two in each
zone — and a second group of three running etcd for them to find each other
through.

```
                     internet
                         │
                    ┌────┴────┐  {stack}-alb, HTTP/80, internet facing
                    │   ALB   │  health check GET /asyncdb/health
                    └────┬────┘
        ┌────────────────┼────────────────┐        one private subnet per AZ
        │                │                │
   ┌────┴────┐      ┌────┴────┐      ┌────┴────┐   AutoScalingGroup, desired 6
   │ asyncdb │      │ asyncdb │      │ asyncdb │   nginx :80, API :8080
   │ asyncdb │──────│ asyncdb │──────│ asyncdb │   two per zone, holding half
   └────┬────┘      └────┬────┘      └────┬────┘   the keyspace each
        └────────────────┼────────────────┘
                         │  ASYNCDB_ETCD names all three, found at boot
        ┌────────────────┼────────────────┐
        │                │                │
   ┌────┴────┐      ┌────┴────┐      ┌────┴────┐   EtcdAutoScalingGroup, 3
   │  etcd   │──────│  etcd   │──────│  etcd   │   :2379 clients, :2380 peers
   └─────────┘      └─────────┘      └─────────┘   DescribeInstances, no fixed
                                                   addresses either side
```

The shape is the one [the cluster](/database/cluster) describes — several
instances, one etcd, no leader — with the load balancer in front so that a
client can ask any of them, which is exactly what the cluster is for: every node
answers for every key. Two instances per availability zone, and
[one copy of every record in each zone](/database/cluster#one-copy-in-every-zone)
split between that zone's two nodes, so the node the load balancer picked holds
the key half the time and asks its partner in the same zone the rest of the
time.

## Driving it

The `Makefile` is four lines over the AWS CLI, and there is nothing else to
install:

```bash
make create-stack     # aws cloudformation create-stack --stack-name asyncdb
make update-stack     # the same template again, in place
make describe-stack   # the stack events, which is where a failure says why
make delete-stack     # everything above, gone
```

Each of them names `--template-body file://cloudformation.yaml`, so the file in
the working tree is what is deployed — there is no bucket and no packaging step.
`create-stack` and `update-stack` pass `--capabilities CAPABILITY_NAMED_IAM`
because the template creates roles.

The same four lines are what the build drives on a push to `master`: it creates
the stack, waits for `/health` to name six nodes, runs the
[Postman collection](https://github.com/martinrixham/asyncdb/tree/master/api)
against the `Url` output with `newman`, and then deletes the stack, whether the
collection passed or not. A failing assertion fails the build. Because the stack
[cannot be deployed twice in one region](#what-this-stack-does-not-do), a stack
left standing by hand makes that `create-stack` fail — and the build then tears
down nothing, because it deletes only a stack it created itself.

## The address

The stack has one output, `Url`: the load balancer's DNS name with `http://` in
front of it. That single address is the whole deployment — the UI in a browser,
and the API under `/asyncdb` — because every node
[answers for every key](/database/cluster), so there is nothing to choose
between the six instances behind it.

```bash
aws cloudformation describe-stacks --stack-name asyncdb \
  --query 'Stacks[0].Outputs[?OutputKey==`Url`].OutputValue' --output text
```

It is an output and not an export: it is there to be read after a deploy, not to
be `Fn::ImportValue`d by another stack. The value only exists once the
`ApplicationLoadBalancer` is created, so `describe-stacks` gives it while the
stack is still `CREATE_IN_PROGRESS` only after that resource is done — and the
address answers later still, when the first instance passes its health check.

The stack takes no region of its own: it goes wherever the CLI is pointed. Two
things inside it do not, and are written out in the instances' user data —
the ECR registry `332187735950.dkr.ecr.eu-west-2.amazonaws.com` and the
`--region eu-west-2` of the login. **Deploy it anywhere else and the instances
come up and pull nothing.**

## Parameters

| Parameter | Default | Is |
| --- | --- | --- |
| `InstanceType` | `t3.micro` | Used for both groups — the database instances and the etcd instances |
| `BaseAmi` | `/aws/service/ecs/optimized-ami/amazon-linux-2023/recommended/image_id` | A **public** parameter of AWS's, resolved at deploy time to the image both tiers launch from |
| `Version` | `/asyncdb/version` | An SSM parameter of this account's, resolved at deploy time to the asyncdb tag the database instances pull |
| `EtcdVersion` | `/asyncdb/etcd` | The same, for the etcd tag the etcd instances pull out of this account's registry |
| `Nodes` | `6` | How many database instances the group runs, which is [how many ways each zone splits its copy](/deployment/database#the-two-parameters-that-are-the-shape). 3, 6 or 9 |
| `Zones` | `3` | How many availability zones the database tier spans, which is **how many copies of the keyspace there are**. 2 or 3 |

`BaseAmi` is the **ECS-optimised Amazon Linux 2023** image, and the
ECS-optimised part is used **for Docker, not for ECS**: there is no ECS cluster
in the template, no task definition and no agent doing anything; it is simply the
Amazon Linux with a Docker daemon already installed, so the user data can go
straight to `docker run`. It is **2023** rather than 2, which went end of life in
June 2025; what that costs is an IMDSv2 token for the metadata read, since 2023's
AMIs take no unauthenticated metadata request.

**One parameter, for both tiers, and it is AWS's rather than this account's.**
There is no image of our own to bake: host preparation is two lines of user data,
the etcd container comes off the boot path through
[the mirror](/pipeline/#mirroring-etcd) rather than through an image, and a patch
level baked into an image goes stale the day it is taken, against a base image
AWS republishes.

The cost of naming AWS's parameter is real and worth stating: **two instances of
the same auto scaling group launched a fortnight apart can be two different
operating systems**, because an instance replacement picks up whatever is current
and nothing here pins it. That is accepted rather than overlooked — a node holds
no state that outlives it, [the tier is replaced rather than
patched](/deployment/database#the-root-volume), and pinning it is one parameter
override away.
parameter override away.

## What is in the stack

| Page | Resources |
| --- | --- |
| [The network](/deployment/network) | `VPC`, `InternetGateway`, `AttachGateway`, `PublicSubnet1`–`3`, `PrivateSubnet1`–`3`, `PublicRouteTable`, `PublicRoute`, `PrivateRouteTable`, `PublicSubnetRouteTableAssociation1`–`3`, `PrivateSubnetRouteTableAssociation1`–`3`, `Ipv6CidrBlock`, `EgressOnlyInternetGateway`, `PrivateRoute`, `ALBSecurityGroup`, `InstanceSecurityGroup`, `EtcdSecurityGroup`, `InstanceApiIngress`, `EtcdPeerIngress`, `EtcdClientIngress` |
| [The database tier](/deployment/database) | `InstanceRole`, `InstanceProfile`, `LaunchTemplate`, `AutoScalingGroup`, `ApplicationLoadBalancer`, `ALBTargetGroup`, `ALBListener` |
| [The etcd tier](/deployment/etcd) | `EtcdRole`, `EtcdInstanceProfile`, `EtcdLaunchTemplate`, `EtcdAutoScalingGroup` |
| [What it costs](/deployment/cost) | All of the above, priced — and what a read, a write and a scan add to it |

## Before the first deploy

Three things the template needs and does not create. **A key pair is not one of
them**: the instances are in
[private subnets with no SSH](/deployment/network#getting-onto-an-instance) and
neither tier sets `KeyName`. Neither is the `asyncdb` repository: it is
in `eu-west-2`, it is where [the release](#the-image-the-instances-pull) pushes,
it is no part of this stack — and
[the build creates it](/pipeline/#making-the-repositories) if it is not there,
on every push and not only on a release.

- **An SSM parameter named `/asyncdb/version`**, in `eu-west-2`, holding that
  tag. The `Version` parameter resolves it at deploy time, so a stack operation
  fails outright if it does not exist. The release writes it; before the first
  release there has been no write, so create it by hand:

  ```bash
  aws ssm put-parameter --name /asyncdb/version --type String \
    --value "$(cat version)" --overwrite
  ```

- **An ECR repository named `etcd` and an SSM parameter named `/asyncdb/etcd`**,
  the same arrangement one tier down: `EtcdVersion` resolves the parameter at
  deploy time and the etcd instances pull the tag it names. Neither is a thing to
  create by hand in the ordinary case — [the build makes both](/pipeline/#mirroring-etcd)
  before it deploys, on every push and not only on a release — so this is a
  precondition only for a stack stood up before CI has ever run against the
  account.
- **The image tag itself**, pushed under that name. An instance that cannot pull
  simply has no container: `docker run` fails and the load balancer takes the
  instance out of service on the health check. The group's health check is
  [`EC2`](/runbook/deployment#the-group-does-not-replace-a-failed-application), so
  it does not replace it either — the instance sits there running nothing until
  a hand terminates it, and the load balancer answers 502 for as long as no
  instance has a container.

## The image the instances pull

The instances run the image the `Dockerfile` builds — nginx on port 80 serving
the built UI and reverse-proxying `/asyncdb/*` to the `asyncdb` binary on port
8080 — and they pull it from ECR by tag.

The tag is the contents of the `version` file at the root of the repository.
Pushing to `master` runs `.github/workflows/build.yaml`, which builds the image,
asks ECR whether that tag exists, and only if it does not pushes it, writes the
tag to `/asyncdb/version` and git-tags the commit. So a release is: change
`version`, push, then `make update-stack` and replace the instances.

**`version` is the only place the tag is written by hand.** The template does not
name it: the `Version` parameter reads `/asyncdb/version`, and the deploy
therefore picks up what CI actually published rather than whatever the working
tree happens to hold. Passing the parameter explicitly means passing the
*parameter name*, never the tag — which is why the `Makefile` passes nothing at
all.

The workflow needs `ssm:PutParameter` on `arn:aws:ssm:eu-west-2:*:parameter/asyncdb/*`
for that write. It runs before the git tag, so if the grant is missing the image
is pushed and the commit is left untagged.

## Rolling out a new version

`update-stack` with a changed `LaunchTemplate` creates a new version of the
launch template, and the auto scaling group's `Version` is
`LatestVersionNumber`, so it picks the new one up. It does **not** replace the
instances that are already running: there is no `UpdatePolicy` and no instance
refresh in the template, so the change reaches an instance when that instance is
replaced. Either start an instance refresh by hand, or terminate the instances
one at a time and let the group replace them.

Because [records do not move](/database/cluster#what-this-is-not), rolling
instances is not free once there is more than one node: a replacement is an
empty database, and its zone's copy of the keys it owns is gone until they are
written again. The other zones still hold theirs, so reads are answered and the
window is a lost copy rather than lost data — but roll one instance at a time
and let each come back before the next goes.

## What this stack does not do

It is a small template, and it is worth being plain about where it stops.

- **There is nothing durable.** RocksDB lives at `/var/lib/asyncdb`, which the
  container binds from the [root volume](/deployment/database#the-root-volume) of
  an instance the auto scaling group is free to replace. Binding it is what makes
  a *restarted container* open the store it wrote rather than an empty one, and
  the container is run `--restart always` so that a crash is a restart. The volume
  is EBS and declared by the launch template, but `DeleteOnTermination` is true:
  no snapshot, no backup, and **a replaced instance is still an empty database**,
  which answers [`table_not_found`](/database/cluster#what-this-does-not-do) for
  the keys it owns until the tables are declared again. What survives an instance
  is the [copy each other zone holds](/database/cluster#one-copy-in-every-zone),
  and nothing rebuilds the one that went with it; what survives the stack is
  nothing.
- **The stack can be deployed more than once in a region, and the quotas are
  what stop it.** Nothing in the template is named by hand except the load
  balancer, which is named after the stack, so the name of the stack is the whole
  of what makes two of them different. What they share is the ECR repository they
  pull from and the two SSM parameters that name the tags — so two stacks run the
  same version, and there is no way to stand up two versions at once. **Each one
  is a VPC, a load balancer and nine `t3.micro`**, against a default quota of five
  VPCs to a region; [the pipeline](/pipeline/#the-shares) runs three at a time and
  that is what sizes the account. The [`Url` output](#the-address) is how to tell
  them apart.
- **There is no HTTPS.** The listener is HTTP on port 80, in and out. There is
  no certificate, no redirect and no `Scheme: internal` anywhere: everything the
  API carries crosses the internet in the clear.
- **Nothing scales anything.** The application group takes its capacity from
  the `Nodes` parameter, 6 by default, with no scaling policy, no alarm and no
  target tracking, and the etcd group is 3 pinned between 1 and 3. The parameter
  is there for a hand to move — and growing the group is
  [a thing to do deliberately](/database/cluster#what-this-is-not), because a
  key that changes owner is a key the new owner does not have.
- **Losing two etcd instances at once is still a hand on the keyboard.** A group
  replaces a dead instance and the replacement
  [admits itself to the cluster](/deployment/etcd#the-member-dance-automated),
  which heals the common failure — but a cluster with no quorum cannot admit
  anybody, and the replacements
  [refuse to start rather than split the membership](/deployment/etcd#what-a-failure-looks-like).
- **Nothing caps the CPU bill.** `t3.micro` is burstable and the template sets no
  `CreditSpecification`, so both tiers take the T3 default of `unlimited`: an
  instance that runs out of credits keeps running at full speed and charges for
  the surplus. Three saturated database instances cost more in credits than
  [the whole stack costs standing still](/deployment/cost#cpu-credits).
- **`ec2:DescribeInstances` is granted on `"Resource": "*"`** to both tiers,
  because the action takes no resource. The `vpc-id` filter in the query is what
  makes the answer this stack's; the permission is the account's.
