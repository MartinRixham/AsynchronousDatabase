# The deployment

The AWS stack is one template, one load balancer, six database instances in an
auto scaling group and three etcd instances in a group of their own, each tier
finding the other with `DescribeInstances`. Most of what
fails here fails at *launch* — an instance that never comes up — and it fails
quietly, because an instance with no container says nothing about why.

[The deployment pages](/deployment/) describe the stack. This one is what to do
when it misbehaves.

## Asking one instance

There is one address for six nodes and no session stickiness, so the load
balancer picks a different instance each time. That is what makes `/health`
useful — and awkward, because a single call is a single node's view:

```bash
URL=$(aws cloudformation describe-stacks --stack-name asyncdb \
  --query 'Stacks[0].Outputs[?OutputKey==`Url`].OutputValue' --output text)

for i in $(seq 1 12); do
  curl -s --max-time 5 "$URL/asyncdb/health" | jq -c '{n: (.nodes|length), leads, write_stalled}'
done
```

Twelve calls over six instances will usually have reached all of them. To reach
one instance in particular, go to the instance: the API port is `8080` on a
private address and is not behind the load balancer at all.

```bash
aws ec2 describe-instances --filters Name=tag:Name,Values=asyncdb \
  Name=instance-state-name,Values=running \
  --query 'Reservations[].Instances[].[InstanceId,PrivateIpAddress,Placement.AvailabilityZone]' \
  --output table
```

## The whole deployment answers 502

Every instance is failing its health check, or none is in service. In order of
likelihood:

1. **The image tag cannot be pulled.** The commonest cause, and the one that
   sustains itself — see below.
2. **etcd is not up**, so nothing joined. The database still answers, so this
   does *not* cause a 502 on its own; if `/health` answers with a `nodes` naming
   only the node that answered, that is what is happening, and reads and writes are being served by
   [six separate cluster-of-one instances](/runbook/membership#etcd-cannot-be-reached).
   That is worse than a 502 and looks better.
3. **The stack was created in the wrong region.** The registry
   `332187735950.dkr.ecr.eu-west-2.amazonaws.com` and the `--region eu-west-2` of
   the login are written into the user data. Deploy anywhere else and the
   instances come up and pull nothing.
4. **The `Version` parameter resolved to a tag that was never pushed.**

**Check the target group first** — it says whether the instances are failing or
absent:

```bash
aws elbv2 describe-target-health --target-group-arn "$(
  aws elbv2 describe-target-groups --query 'TargetGroups[?contains(TargetGroupName,`asyncdb`)].TargetGroupArn' \
    --output text)" --output table
```

Then go onto an instance and look at the container. The instances are in
[private subnets with no SSH](/deployment/network#getting-onto-an-instance), so
the way in is Session Manager:

```bash
aws ssm start-session --target i-0123456789abcdef0
```

```bash
sudo docker ps -a
sudo docker logs $(sudo docker ps -aq | head -1)
sudo cat /var/log/cloud-init-output.log
```

`cloud-init-output.log` is where a failed `docker pull` or a failed `docker
login` actually appears. Nothing else records it.

## An instance never comes up

The user data has to finish inside `HealthCheckGracePeriod`, which is **200
seconds** on the database tier and 300 on the etcd tier, measured from the launch
and not from the first check. Into the 200 has to fit a `docker login`, a cold
`docker pull` of the image, and the
[`DescribeInstances` that finds etcd](/deployment/etcd#how-the-database-tier-finds-it)
— which answers at once when the etcd tier is up and waits **up to a minute**
when it is not.

**Three and a bit minutes is not a generous margin for that sequence.** It holds
because the pull is from ECR in the same region and the discovery call normally
returns immediately, but a slow endpoint or a larger image eats it, and the
failure looks like an instance that never comes up rather than like a timeout.
There is room to raise it — nothing waits on the grace period except the first
health check of a genuinely dead instance. Nothing in the user data updates
packages or installs the AWS CLI
([by design](/deployment/database#the-launch-template)): Amazon Linux 2023 ships
the CLI, and the base image is taken as AWS publishes it rather than patched at
boot, so neither costs anything here.

## The group does not replace a failed application

Worth knowing exactly, because it decides whether an outage heals itself.

`HealthCheckType` on the auto scaling group is **`EC2`**, so the group replaces
an instance whose *instance* has failed — a failed EC2 status check — and **not**
one whose *application* has failed. An instance whose container exited, or whose
pull failed, is one the load balancer stops sending traffic to and that the group
**leaves running**.

What follows from `EC2`, with `--restart always` alongside it:

| What failed | What happens |
| --- | --- |
| The `asyncdb` process crashed | The container restarts. The store survives; the node rejoins |
| The container was stopped | It restarts |
| The image could never be pulled | **Nothing happens.** There is no container to restart and the group does not replace the instance. It sits out of service until a hand terminates it |
| The instance itself failed | The group replaces it — which is [an empty database](/runbook/storage#a-node-came-back-empty) |

So the recovery for an instance that came up with no container is to
**terminate it yourself** and let the group launch a replacement:

```bash
aws autoscaling terminate-instance-in-auto-scaling-group \
  --instance-id i-0123456789abcdef0 --should-decrement-desired-capacity
```

Do that only after fixing whatever stopped the pull, or the replacement starts
the same work from the beginning.

## Instances are replaced in a loop

Under the `EC2` health check this is an *instance* fault rather than an
application one, so it is rare — but the group will still churn if instances are
failing their EC2 status checks, and any manual instance refresh will churn if
the image cannot be pulled.
refresh will churn if the image cannot be pulled.

**Stop the churn before diagnosing it**, because every replacement is another
empty database:

```bash
aws autoscaling suspend-processes --auto-scaling-group-name <name> \
  --scaling-processes ReplaceUnhealthy
```

Then read `cloud-init-output.log` on one of the survivors, fix the cause, and
resume. The usual causes are the pull and the grace period.

## The stack will not create

| Message | Is |
| --- | --- |
| `{stack}-alb` already exists | A stack of that name is already standing. Load balancer names are unique to a region and this one is the stack's, so **a stack cannot be created over itself** — a second stack under another name is fine |
| `The maximum number of VPCs has been reached` | The region is full. Each stack is a VPC and the default quota is five; [the pipeline](/pipeline/#the-shares) uses four of them for the length of a run, which leaves room for the default VPC and nothing else |
| Parameter `/asyncdb/version` not found | The SSM parameter does not exist. CloudFormation cannot resolve it, so the operation fails outright |
| The group reports a failed activity, not a template error | Something the launch template names is missing — the AMI, the instance profile or the image |
| An etcd instance has no container | The tag `/asyncdb/etcd` names is not in this account's ECR. The boot script pulls [what the build mirrored](/pipeline/#mirroring-etcd) and never quay.io — or the node found a cluster it could not join, which is [a quorum failure](/runbook/membership#etcd-has-lost-quorum) and deliberate |
| Either tier came up in a cluster of one | `ec2:DescribeInstances` did not answer. [The route out](/deployment/network#the-route-out) has to be there before an instance boots, `AWS_USE_DUALSTACK_ENDPOINT` has to be set so the call goes to an endpoint IPv6 reaches, and the instance profile has to carry the `discovery` policy |
| The database instances have no container | The pull. `docker login` and `docker pull` go out over [the route out](/deployment/network#the-route-out), and both have to name ECR's dual-stack form — `ecr.eu-west-2.api.aws` for the login and `…dkr-ecr.eu-west-2.on.aws` for the registry — because there is no IPv4 route to the ordinary names |

Three things the template needs and does not create:
**an ECR repository `asyncdb` in `eu-west-2`**, **the SSM parameter
`/asyncdb/version`**, and **the image tag itself pushed under that name**. A key
pair is not one of them.

```bash
aws ssm put-parameter --name /asyncdb/version --type String \
  --value "$(cat version)" --overwrite
```

`make describe-stack` is where a failure says why — it prints the stack events,
and the first `CREATE_FAILED` in them is the answer.

A stack left standing by hand also breaks CI: the build creates the stack, and
because it tears down only a stack **that same run created**, a standing stack
makes `create-stack` fail and is then left alone.

## The release did not reach the instances

**A `LaunchTemplate` change does not recycle running instances.** There is no
`UpdatePolicy` and no instance refresh in the template, so `update-stack` alone
changes nothing about what is running: the change reaches an instance when that
instance is replaced.

**Rolling instances is not free.** A replacement is an empty database, and its
zone's copy of the keys it owns is gone until they are written again. The other
zones still hold theirs, so reads are answered and the window is a lost copy
rather than lost data — but:

> **Roll one instance at a time, and let each come back before the next goes.**

Capacity is worth moving three at a time — one per zone — so that no zone holds
a larger share than the others.

## The version did not publish

The push to ECR is in `publish`, which runs **only if the tag in
`version` has not passed the suite already** — that is, only if the remote has no
`{version}` git tag. Leaving `version` unchanged makes CI a complete build whose
result is thrown away, which is a no-op publish and not a failure.

| Symptom | Is |
| --- | --- |
| No new image, build green | `version` was not bumped, so the publishing job was skipped |
| Image pushed, commit not tagged | The workflow lacks `ssm:PutParameter` on `arn:aws:ssm:eu-west-2:*:parameter/asyncdb/*`. The write runs before the git tag |
| Instances still on the old tag | Expected. Replace them — see above |

`version` is the only place the tag is written by hand. The template's `Version`
parameter reads `/asyncdb/version`, so a deploy picks up what CI actually
published rather than the working tree — which is why the `Makefile` passes no
parameter, and why passing one means passing the **parameter name** and never
the tag.

## The build failed after the stack came up

`publish` publishes the image, and the three `verify` shares each stand a stack
up with it and run their share against it: wait for `/health` to name six nodes,
then — on the one share that carries them — `newman`, the Playwright journeys and
`perf/write.sh` and `perf/read.sh`, and on every share its own chaos experiments.
A share deletes its stack afterwards **whether it passed or not**, so a failure
leaves nothing running and nothing to inspect.

| Step | A failure means |
| --- | --- |
| Wait for the cluster | Six nodes never answered inside 90 attempts at 10 seconds — an instance never came up, or etcd did not |
| `newman` | An API assertion. The folders are ordered and depend on each other, so read the first failure and not the last |
| Playwright | A UI journey. The report is uploaded as an artifact |
| `perf/*.sh` | **A request that was not 2xx** — including the `000` of a transfer that never answered. Latency is reported and never asserted on, so a perf failure is an availability failure |

To reproduce any of them, stand the stack up by hand with `make create-stack` and
run the same command against the `Url` output. Remember that the collection
expects `clusterSize` to be **6** for the AWS stack, 3 for compose and 1 for a
lone instance.

## What the deployment does not do

- **Nothing is durable.** No snapshot, no backup, and a replaced instance is an
  empty database.
- **Nothing replaces a dead etcd instance** — that is
  [the member dance](/runbook/membership#an-etcd-member-was-recreated-and-will-not-rejoin).
- **Nothing scales anything.** `DesiredCapacity: 6` between 1 and 7, no policy
  and no alarm.
- **There is no HTTPS.** Everything the API carries crosses the internet in the
  clear.
- **Nothing caps the CPU bill.** `t3.micro` with no `CreditSpecification` takes
  the T3 default of `unlimited`, so a saturated instance charges for the surplus
  rather than slowing down.
