# Chaos

The failure modes in [`doc/runbook`](../doc/runbook), injected into the deployed stack with the
AWS Fault Injection Service and asserted on from outside. The runbook says what each fault looks
like, what recovers by itself and what needs a hand; this is what checks that it still does.

It is a test suite and not a demonstration. Every experiment ends by asserting that what it
broke came back, and an assertion that did not hold is a non-zero exit — which is what lets
`build.yaml` run it last against the deployed stack and fail the build on it.

## The experiments

| Experiment | The failure mode | The fault |
| --- | --- | --- |
| `node-stops` | [A node does not answer](../doc/runbook/nodes.md), [an instance was replaced](../doc/runbook/nodes.md), [the rebuild](../doc/runbook/rebuild.md) | `aws:ec2:stop-instances`, one database node |
| `zone-lost` | [A read needs one copy](../doc/runbook/index.md), [fewer zones than the deployment has](../doc/runbook/membership.md) | `aws:network:disrupt-connectivity`, scope `availability-zone` |
| `scan-loses-a-node` | [A scan fails while everything else works](../doc/runbook/nodes.md) | A `DOCKER-USER` rule rejecting what arrives for port 8080, one node **per zone** |
| `etcd-unreachable` | [etcd cannot be reached](../doc/runbook/membership.md) — one node, cluster of one | A `DOCKER-USER` rule rejecting what the container sends to port 2379 |
| `node-latency` | [A node that is up but wrong](../doc/runbook/nodes.md), [threads are all waiting](../doc/runbook/nodes.md) | `AWSFIS-Run-Network-Latency-Sources` toward the VPC |
| `disk-fills` | [RocksDB returned an error](../doc/runbook/storage.md), [the disk is filling](../doc/runbook/storage.md) | `AWSFIS-Run-Disk-Fill`, the whole volume |
| `etcd-quorum-lost` | [etcd has lost quorum](../doc/runbook/membership.md) | `aws:ec2:stop-instances`, two of the three members |

The order is the order they run in, and it is not arbitrary. The agentless faults come first
because they need nothing of the instances themselves. `etcd-quorum-lost` is last because it is
the only one that leaves the cluster having been *wrong about itself* rather than merely short
of a node, and the pipeline deletes the stack next.

`node-stops` is the one to run if only one is run. It is the only test anywhere of
[the rebuild](../doc/runbook/rebuild.md), and the rebuild is the only thing in the system that
puts a lost copy back.

## Run it

The stack has to be up, and the suite refuses to start against one that is not already whole —
six nodes in three zones, nothing stalled. Chaos against a cluster that is already broken proves
nothing.

```bash
make create-stack                    # or against the stack a build stood up
make create-chaos-stack              # the role FIS assumes
chaos/run.sh
make delete-chaos-stack
```

A subset, in the order given:

```bash
CHAOS_EXPERIMENTS='zone-lost node-stops' chaos/run.sh
```

One experiment on its own, which is how to read one while it runs:

```bash
chaos/node-stops.sh
```

### What it costs

Roughly twenty minutes for the first three, most of it `node-stops` waiting for the auto
scaling group to launch a replacement and for that replacement to rebuild itself, and about
half an hour again for the four that go in through SSM — four or five minutes of fault each,
and a settle after every one of them. The faults themselves are minutes; the waiting is the
deployment's own timings — a ten second lease, a sixty second load balancer health check, a two
hundred second grace period — and `CHAOS_SETTLE` and `CHAOS_RECOVERY` are how much of each is
allowed for.

## The role, and the permission to use it

`chaos.yaml` is a stack of its own holding two things, and it is separate from
`cloudformation.yaml` deliberately: neither of them is a thing to leave standing beside a
database, and this way both are created for a run and deleted after it.

| Resource | Is |
| --- | --- |
| `ChaosRole` | The role FIS assumes, with the three `AWSFaultInjectionSimulator*Access` managed policies on it |
| `ChaosPolicy` | What lets the credentials **running the suite** drive FIS at all: the eleven `fis:` actions it uses, and `iam:PassRole` on `ChaosRole` alone, conditioned to `fis.amazonaws.com` |

`ChaosPolicy` attaches to the IAM groups named by the `Operators` parameter, which defaults to
`builders` — the group holding the identity the pipeline runs as. Nothing else about the account
grants FIS anything, which is deliberate: outside a chaos run, nobody in this account can start
an experiment.

`CHAOS_ROLE` overrides the role if it is managed somewhere else.

The rest of what the suite needs — `ec2:DescribeInstances`, `ssm:SendCommand`,
`autoscaling:SuspendProcesses` — the build's credentials already have.

## Validate before you run

```bash
chaos/validate.sh
```

Every experiment's template, created and deleted again, and nothing started. The service checks
every action, parameter and target arn when a template is created, so this is the whole of
"would this experiment run?" for two API calls each, no fault, and seconds — where finding the
same mistake by running the suite is the length of the experiment that hits it. It needs the
stack up, because the arns in a template name real instances and subnets.

It is worth running after any change to an experiment. Two mistakes that cost a full run to find
would have been caught by it in seconds: a template the credentials were not allowed to create,
and `completeIfInstancesTerminated` without the `startInstancesAfterDuration` the service
insists goes with it.

## The faults that go in through SSM

Four of the seven inject their fault with `aws:ssm:send-command`, and they run by default like
the other three. They did not always: the documents they run install what they need — `atd` for
the rollback timer, `tc` for the latency — from the distribution's own repositories, and for as
long as [the network](../doc/deployment/network.md) was seven VPC endpoints and no route out, the
four failed at their precondition and were skipped behind a `CHAOS_SSM=1` that nobody set.

The endpoints are [gone](../doc/deployment/network.md#the-route-out). What replaced them is an
egress-only internet gateway, which is a route to the internet that only opens outwards and only
over IPv6 — so `dnf` reaches the Amazon Linux repositories, the SSM agent reaches Systems
Manager over its dual-stack endpoint, and the documents do what they say. The cost of that is
paid in the deployment rather than here: **the faults now depend on an instance being able to
install a package while it is under test**, and a repository that does not answer is an
experiment that fails at its precondition rather than an assertion that did not hold. FIS
reports that as an experiment that `failed` with a reason, which the harness prints.

The other three need none of it, because the fault is a network access control list or an
instance state that the service changes from outside. That is why they are still first.

### Two of the four are a rule of our own

`node-latency` and `disk-fills` run `AWSFIS-Run-Network-Latency-Sources` and
`AWSFIS-Run-Disk-Fill`. `scan-loses-a-node` and `etcd-unreachable` run `AWS-RunShellScript` and a
script `blackhole_parameters` in [`harness.sh`](harness.sh) builds, because
**`AWSFIS-Run-Network-Blackhole-Port` blocks nothing here**: it writes its rules into `INPUT` and
`OUTPUT`, and asyncdb is a container behind a published port. Everything a peer sends it is
translated and forwarded, so it goes through `FORWARD` and never `INPUT`; everything the container
sends is forwarded too, and never `OUTPUT`. Both experiments ran the document, watched it report
success, and asserted against a cluster in which nothing at all had happened — three nodes that
were meant to be deaf answered a scan and refused none of the writes, and a node that was meant to
have lost etcd renewed its lease throughout. `DOCKER-USER` is the chain docker leaves in `FORWARD`
for exactly this, and it is the one a container's traffic passes through.

The rule **rejects** rather than drops. A node waits thirty seconds on another node, so a dropped
packet is a node that hangs and a reset is a node that does not answer, which is what these two
are about — the copy that does answer is asked next, and `node-latency` is the experiment about
waiting. The script takes the rule out again when its time is up, when it is signalled, and from a
`setsid` of its own if it is killed outright, which is the same belt and braces the `AWSFIS-Run-*`
documents get from `at`.

`node-latency` needs none of this, because `tc` shapes the host's own interface and a container's
traffic leaves through it like anything else. That is why it was the one SSM experiment that
passed while the other three were asserting against faults that were never injected.

## What this does not cover

Not everything in the runbook is a fault an infrastructure service can inject, and the ones that
are not are worth naming so that nobody looks here for them:

| Not covered | Why |
| --- | --- |
| [`Corruption` in RocksDB](../doc/runbook/storage.md) | Nothing in FIS damages a file. `aws:ebs:pause-io` is io2 Block Express and these are `gp3` root volumes |
| [The store will not open](../doc/runbook/storage.md) | Two processes over one directory is a host-local lock conflict, not an infrastructure fault |
| [A node in no zone](../doc/runbook/membership.md) | A metadata read that failed at boot. A configuration fault |
| [`invalid_cursor`, `stale_leader`, `table_not_found`](../doc/runbook/errors.md) | Client-level. That is what [`api/`](../api) asserts |
| [A delete during a rebuild](../doc/runbook/rebuild.md) | FIS can terminate the instance; the racing delete needs a harness timed against it |
| [Everything about the release](../doc/runbook/deployment.md) | The version gate, the stack that will not create, the tag that never published. Not runtime faults |

Two things it covers and reports rather than asserts. The first is that **a full disk breaks the
proxy before it breaks the store**. nginx spools a request body over 8 KiB to a temporary file, so
on a full volume it answers `500 unavailable` out of `50x.json` and the database is never asked —
`disk-fills` therefore writes in two sizes, a megabyte that the proxy refuses and a kilobyte that
reaches RocksDB, and asserts only that every refusal carries a code a client can branch on. What
the store itself does with the kilobytes is printed, not asserted: RocksDB preallocates its write
ahead log, so a node whose volume filled a minute ago still has tens of megabytes reserved to write
into, and whether `No space left on device` arrives inside one fault is the deployment's timing
rather than the database's behaviour. What is asserted instead is the part that matters and no
error code can say: every write that answered `2xx` while the disk was full is still there
afterwards.

The second is that **the runbook overstates what survives**
[a node that does not answer in every zone](../doc/runbook/nodes.md). It says reads and writes of
individual keys are fine there, and neither is quite true. A write needs *every* copy, so roughly
seven writes in eight touch one of the three deaf nodes and are refused; `scan-loses-a-node`
prints that number rather than asserting on it. And a read needs one copy that answers, but the
copies of a partition are one node per zone and one node per zone is what has gone deaf — so one
partition in eight has every copy of it deaf, and those keys are read only by a request the load
balancer happens to send to one of the deaf nodes themselves, which are still in service and still
hold them. The experiment asserts that every key is read, and gives the load balancer the attempts
it takes to come round to them.

## Everything is an environment variable

As in [`perf/`](../perf).

| Variable | Is | Default |
| --- | --- | --- |
| `CHAOS_EXPERIMENTS` | Which experiments, in what order | all seven |
| `CHAOS_STACK` | The stack under test | `asyncdb` |
| `CHAOS_ROLE_STACK` | The stack holding the FIS role | `asyncdb-chaos` |
| `CHAOS_ROLE` | The role arn, instead of that stack's output | |
| `CHAOS_URL` | The address to drive, instead of the stack's `Url` output | |
| `CHAOS_TABLE` | The table the suite seeds and reads | `chaos` |
| `CHAOS_RECORDS` | How many records it seeds | 200 |
| `CHAOS_SETTLE` | How long a membership change is given | 150 seconds |
| `CHAOS_RECOVERY` | How long an instance replacement is given | 900 seconds |
| `CHAOS_ONSET` | How long a started fault is given to bite | 20 seconds |

Each experiment has one or two of its own — the length of its fault, the size of its latency —
named at the top of the script that uses it.

**A duration is a ceiling, not the bill.** FIS charges per action-minute of an action that
actually ran, and `fis_stop_now` takes each fault away as soon as that experiment's assertions are
done rather than watching it expire — so five of the seven are billed for what they used. That is
what lets the durations stay generous: a fault that expires mid-assertion is a *false failure*
rather than a weaker test, because `scan-loses-a-node` asserts that a scan **fails** while a node
is deaf and goes red if the node comes back early. Shortening one of those five saves nothing now.

A stopped experiment removes its own fault, and by three different routes. The agentless actions
undo what they installed; the `AWSFIS-Run-*` documents roll back when their command is cancelled;
and the script in `blackhole_parameters` traps the `TERM` that the cancellation sends and deletes
its rule, which is what that trap is for — its detached safety net still fires later regardless,
and removing a rule that is already gone is nothing. None of it is taken on trust: the recovery
assertion that every experiment runs immediately afterwards is the check that the fault went.

**Two still pay their whole duration.** `node-stops` has no duration on its action at all — it
completes when the instance stops, and is one action-minute whatever the assertions do.
`etcd-quorum-lost` stays on `fis_await_end` deliberately: what starts the etcd members again is
the action's own `startInstancesAfterDuration`, and whether an early stop honours it is not a
thing to discover on the last experiment of a run. `CHAOS_ETCD_DURATION` is therefore a real
duration rather than a ceiling, and the pipeline sets it to `PT3M`.

A duration in seconds becomes the FIS action's own duration as `PT$((seconds / 60))M`, so **keep it
a whole number of minutes**: 150 is a document told to run for 150 seconds inside an action billed
and stopped at `PT2M`.

## Reading a run

```
== A node is stopped
   Reads survive it, writes recover with the membership, the replacement rebuilds.

  PASS the stack is running six database nodes
  Stopping i-0a1b2c3d4e5f60718.
  Experiment EXPabc123 from template EXTdef456.
  PASS the fault was injected
  PASS the stopped node left the membership and the zone count did not change
  ---- while the node was going away: 143 of 190 reads answered 2xx
  PASS every read is answered with the node out of the membership
  ...
```

A `PASS`/`FAIL` line is an assertion. A `----` line is something measured and reported and never
asserted on — the reads a client lost while the load balancer had not yet noticed a dead target
are the load balancer's health check interval and not the database, and latency here is
[the same as it is in `perf/`](../perf): reported, never a threshold.
