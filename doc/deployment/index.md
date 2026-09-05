# Deploying to AWS

`cloudformation.json` in the root of the repository is the whole of the
infrastructure: one template, one stack, no modules and no state file. It builds
a VPC across three availability zones, an application load balancer, an auto
scaling group of instances running the asyncdb image from ECR, and three
instances running etcd for them to find each other through.

```
                     internet
                         │
                    ┌────┴────┐  ClusterALB, HTTP/80, internet facing
                    │   ALB   │  health check GET /asyncdb/health
                    └────┬────┘
        ┌────────────────┼────────────────┐        one public subnet per AZ
        │                │                │
   ┌────┴────┐      ┌────┴────┐      ┌────┴────┐   AutoScalingGroup, desired 3
   │ asyncdb │──────│ asyncdb │──────│ asyncdb │   nginx :80, API :8080
   └────┬────┘      └────┬────┘      └────┬────┘   forwarding to each other
        └────────────────┼────────────────┘
                         │  ASYNCDB_ETCD names all three
        ┌────────────────┼────────────────┐
        │                │                │
   ┌────┴────┐      ┌────┴────┐      ┌────┴────┐   Etcd1..3, fixed addresses
   │  etcd   │──────│  etcd   │──────│  etcd   │   :2379 clients, :2380 peers
   └─────────┘      └─────────┘      └─────────┘   10.0.0.10 .1.10 .2.10
```

The shape is the one [the cluster](/database/cluster) describes — several
instances, one etcd, no leader — with the load balancer in front so that a
client can ask any of them, which is exactly what the cluster is for: every node
answers for every key.

## Driving it

The `Makefile` is four lines over the AWS CLI, and there is nothing else to
install:

```bash
make create-stack     # aws cloudformation create-stack --stack-name asyncdb
make update-stack     # the same template again, in place
make describe-stack   # the stack events, which is where a failure says why
make delete-stack     # everything above, gone
```

Each of them names `--template-body file://cloudformation.json`, so the file in
the working tree is what is deployed — there is no bucket and no packaging step.
`create-stack` and `update-stack` pass `--capabilities CAPABILITY_NAMED_IAM`
because the template creates roles.

The same four lines are what the build drives on a push to `master`: it creates
the stack, waits for `/health` to name three nodes, runs the
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
between the three instances behind it.

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
| `ECSAMI` | `/aws/service/ecs/optimized-ami/amazon-linux-2023/recommended/image_id` | An SSM public parameter, resolved at deploy time to the current AMI id |
| `Version` | `/asyncdb/version` | An SSM parameter of this account's, resolved at deploy time to the image tag the instances pull |

The ECS-optimised AMI is used **for Docker, not for ECS**. There is no ECS
cluster in the template, no task definition and no agent doing anything; the AMI
is simply the Amazon Linux with a Docker daemon already installed and running,
so the user data can go straight to `docker run`. It is **Amazon Linux 2023**:
the 2 the stack used to run went end of life in June 2025, and the move is a
parameter default and two lines of user data — `--update` on the CLI install and
an IMDSv2 token for the metadata read, since 2023's AMIs take no unauthenticated
metadata request. Resolving it through SSM rather
than pinning an id is what keeps the template region-independent and stops it
going stale — at the cost of an instance replacement picking up a newer AMI than
its neighbours.

## What is in the stack

| Page | Resources |
| --- | --- |
| [The network](/deployment/network) | `VPC`, `InternetGateway`, `AttachGateway`, `PublicSubnet1`–`3`, `PublicRouteTable`, `PublicRoute`, `SubnetRouteTableAssociation1`–`3`, `ALBSecurityGroup`, `InstanceSecurityGroup`, `EtcdSecurityGroup`, `InstanceApiIngress`, `EtcdPeerIngress` |
| [The database tier](/deployment/database) | `InstanceRole`, `InstanceProfile`, `LaunchTemplate`, `AutoScalingGroup`, `ApplicationLoadBalancer`, `ALBTargetGroup`, `ALBListener` |
| [The etcd tier](/deployment/etcd) | `Etcd1`, `Etcd2`, `Etcd3`, and the `Etcd` mapping their addresses live in |
| [What it costs](/deployment/cost) | All of the above, priced — and what a read, a write and a scan add to it |

## Before the first deploy

Four things the template needs and does not create:

- **A key pair named `asyncdb`.** Both launch templates set
  `KeyName: asyncdb`, and an instance launch with a key pair that does not exist
  fails, which the auto scaling group reports as a failed activity rather than
  as a template error.
- **An ECR repository named `asyncdb`**, in `eu-west-2`, holding the tag the
  user data asks for. The repository is where
  [the release](#the-image-the-instances-pull) pushes, and it is not part of
  this stack.
- **An SSM parameter named `/asyncdb/version`**, in `eu-west-2`, holding that
  tag. The `Version` parameter resolves it at deploy time, so a stack operation
  fails outright if it does not exist. The release writes it; before the first
  release there has been no write, so create it by hand:

  ```bash
  aws ssm put-parameter --name /asyncdb/version --type String \
    --value "$(cat version)" --overwrite
  ```

- **The image tag itself**, pushed under that name. An instance that cannot pull
  simply has no container: `docker run` fails, the load balancer takes the
  instance out of service on the health check, and the group, whose health check
  is `ELB`, replaces it — with the same tag, so an instance that cannot pull is
  replaced by another that cannot either, and the load balancer answers 502 for
  as long as that lasts.

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
instances is not free once there is more than one node: a node that goes away
takes its share of the keys with it until its replacement has them written
again.

## What this stack does not do

It is a small template, and it is worth being plain about where it stops.

- **There is nothing durable.** RocksDB lives in the container's filesystem, on
  the [root volume](/deployment/database#the-root-volume) of an instance the auto
  scaling group is free to replace. The volume is EBS and is declared by the
  launch template, but `DeleteOnTermination` is true and nothing mounts it into
  the container: no volume, no snapshot, no backup, and a replaced instance is an
  empty database.
- **The stack cannot be deployed twice in one region.** `ClusterALB` is a fixed
  name, and so is the ECR repository the instances pull from. The
  [`Url` output](#the-address) tells you where one stack is; a second one in the
  same region fails to create the load balancer at all.
- **There is no HTTPS.** The listener is HTTP on port 80, in and out. There is
  no certificate, no redirect and no `Scheme: internal` anywhere: everything the
  API carries crosses the internet in the clear.
- **Nothing scales anything.** The application group is `DesiredCapacity: 3`
  between 1 and 4 with no scaling policy, no alarm and no target tracking, and
  the etcd tier is three fixed instances. The bounds are there for a hand to
  move — and growing the group is
  [a thing to do deliberately](/database/cluster#what-this-is-not), because a
  key that changes owner is a key the new owner does not have.
- **Nothing replaces a dead etcd instance**, which is the price of fixing their
  addresses. [What that costs](/deployment/etcd#what-a-failure-looks-like) is
  one instance recreated by hand, against a tier whose data rewrites itself in
  seconds.
- **Nothing caps the CPU bill.** `t3.micro` is burstable and the template sets no
  `CreditSpecification`, so both tiers take the T3 default of `unlimited`: an
  instance that runs out of credits keeps running at full speed and charges for
  the surplus. Three saturated database instances cost more in credits than
  [the whole stack costs standing still](/deployment/cost#cpu-credits).
- **The description is stale.** It says "Minimal 2-AZ EC2 cluster"; the template
  builds three subnets in three availability zones.
