# Deploying to AWS

`cloudformation.json` in the root of the repository is the whole of the
infrastructure: one template, one stack, no modules and no state file. It builds
a VPC across three availability zones, an application load balancer, a group of
instances running the asyncdb image from ECR, and a second group of instances
running etcd for them to find each other through.

```
                     internet
                         │
                    ┌────┴────┐  ClusterALB, HTTP/80, internet facing
                    │   ALB   │  health check GET /
                    └────┬────┘
        ┌────────────────┼────────────────┐        one public subnet per AZ
        │                │                │
   ┌────┴────┐      ┌────┴────┐      ┌────┴────┐   AutoScalingGroup, desired 3
   │ asyncdb │      │ asyncdb │      │ asyncdb │   nginx :80, API :8080
   └────┬────┘      └────┬────┘      └────┬────┘
        └────────────────┼────────────────┘
                         │                        (see the gaps below — the
        ┌────────────────┼────────────────┐        instances do not yet talk
        │                │                │        to either)
   ┌────┴────┐      ┌────┴────┐      ┌────┴────┐   EtcdAutoScalingGroup, fixed 3
   │  etcd   │      │  etcd   │      │  etcd   │   :2379 clients, :2380 peers
   └─────────┘      └─────────┘      └─────────┘
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
| [The network](/deployment/network) | `VPC`, `InternetGateway`, `AttachGateway`, `PublicSubnet1`–`3`, `PublicRouteTable`, `PublicRoute`, `SubnetRouteTableAssociation1`–`3`, `ALBSecurityGroup`, `InstanceSecurityGroup`, `EtcdSecurityGroup` |
| [The database tier](/deployment/database) | `InstanceRole`, `InstanceProfile`, `LaunchTemplate`, `AutoScalingGroup`, `ApplicationLoadBalancer`, `ALBTargetGroup`, `ALBListener` |
| [The etcd tier](/deployment/etcd) | `DiscoveryTokenLambdaRole`, `DiscoveryTokenLambda`, `DiscoveryTokenCustomResource`, `EtcdLaunchTemplate`, `EtcdAutoScalingGroup` |

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
  cannot pull simply has no container: `docker run` fails, the instance stays up
  and healthy as far as the auto scaling group is concerned, and the load
  balancer takes it out of service on the health check.

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

- **The database instances are not clustered.** The user data runs
  `docker run -d -p 80:80 $IMAGE` and sets no environment, so neither
  `ASYNCDB_ETCD` nor `ASYNCDB_NODE` is set, and
  [set neither and nothing changes](/database/cluster#turning-it-on): each
  instance registers nothing, owns the whole keyspace and knows about no other
  node. The etcd tier stands there unused. Three instances behind the load
  balancer are therefore three independent databases, and which one answers is
  the load balancer's choice — a key written through the balancer is readable
  only from the instance that took the write.
- **The API port is not published or reachable anyway.** The container publishes
  only 80, and `InstanceSecurityGroup` allows 80 from the load balancer and 22
  from anywhere, and nothing on 8080. Clustering these instances means
  publishing 8080 from the container, opening 8080 to the security group itself,
  and setting both environment variables from instance metadata.
- **There is nothing durable.** RocksDB lives in the container's filesystem, on
  the instance store of an instance the auto scaling group is free to replace.
  No volume, no snapshot, no backup: a replaced instance is an empty database.
- **There are no outputs.** The template exports nothing, so the address to
  visit — the load balancer's DNS name — is found with
  `aws elbv2 describe-load-balancers --names ClusterALB` or in the console.
  `ClusterALB` is also a fixed name, so the stack cannot be deployed twice in
  one region.
- **There is no HTTPS.** The listener is HTTP on port 80, in and out. There is
  no certificate, no redirect and no `Scheme: internal` anywhere: everything the
  API carries crosses the internet in the clear.
- **Nothing scales anything.** The application group is `DesiredCapacity: 3`
  between 1 and 4 with no scaling policy, no alarm and no target tracking, and
  the etcd group is pinned at 3. The bounds are there for a hand to move.
- **The description is stale.** It says "Minimal 2-AZ EC2 cluster"; the template
  builds three subnets in three availability zones.
