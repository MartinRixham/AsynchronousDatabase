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
| `ECSAMI` | `/aws/service/ecs/optimized-ami/amazon-linux-2/recommended/image_id` | An SSM public parameter, resolved at deploy time to the current AMI id |

The ECS-optimised AMI is used **for Docker, not for ECS**. There is no ECS
cluster in the template, no task definition and no agent doing anything; the AMI
is simply the Amazon Linux with a Docker daemon already installed and running,
so the user data can go straight to `docker run`. Resolving it through SSM rather
than pinning an id is what keeps the template region-independent and stops it
going stale — at the cost of an instance replacement picking up a newer AMI than
its neighbours.

## What is in the stack

| Page | Resources |
| --- | --- |
| [The network](/deployment/network) | `VPC`, `InternetGateway`, `AttachGateway`, `PublicSubnet1`–`3`, `PublicRouteTable`, `PublicRoute`, `SubnetRouteTableAssociation1`–`3`, `ALBSecurityGroup`, `InstanceSecurityGroup`, `EtcdSecurityGroup`, `InstanceApiIngress`, `EtcdPeerIngress` |
| [The database tier](/deployment/database) | `InstanceRole`, `InstanceProfile`, `LaunchTemplate`, `AutoScalingGroup`, `ApplicationLoadBalancer`, `ALBTargetGroup`, `ALBListener` |
| [The etcd tier](/deployment/etcd) | `Etcd1`, `Etcd2`, `Etcd3`, and the `Etcd` mapping their addresses live in |

## Before the first deploy

Three things the template needs and does not create:

- **A key pair named `asyncdb`.** Both launch templates set
  `KeyName: asyncdb`, and an instance launch with a key pair that does not exist
  fails, which the auto scaling group reports as a failed activity rather than
  as a template error.
- **An ECR repository named `asyncdb`**, in `eu-west-2`, holding the tag the
  user data asks for. The repository is where
  [the release](#the-image-the-instances-pull) pushes, and it is not part of
  this stack.
- **The image tag itself.** The user data pins `VERSION=0.0.2`. An instance that
  cannot pull simply has no container: `docker run` fails, the load balancer
  takes the instance out of service on the health check, and the group, whose
  health check is `ELB`, replaces it — with the same tag, so an instance that
  cannot pull is replaced by another that cannot either.

## The image the instances pull

The instances run the image the `Dockerfile` builds — nginx on port 80 serving
the built UI and reverse-proxying `/asyncdb/*` to the `asyncdb` binary on port
8080 — and they pull it from ECR by tag.

The tag is the contents of the `version` file at the root of the repository.
Pushing to `master` runs `.github/workflows/build.yaml`, which builds the image,
asks ECR whether that tag exists, and only if it does not tags it and git-tags
the commit. So a release is: change `version`, push, then bump `VERSION` in the
user data of `LaunchTemplate` and `make update-stack`. **The version is in two
places and nothing checks that they agree.**

Note that the `docker push` line in that workflow is currently commented out, so
CI tags the commit and ships no image.

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
  the instance store of an instance the auto scaling group is free to replace.
  No volume, no snapshot, no backup: a replaced instance is an empty database.
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
- **The description is stale.** It says "Minimal 2-AZ EC2 cluster"; the template
  builds three subnets in three availability zones.
