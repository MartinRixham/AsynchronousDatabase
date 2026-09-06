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
| `scan-loses-a-node` | [A scan fails while everything else works](../doc/runbook/nodes.md) | `AWSFIS-Run-Network-Blackhole-Port` on 8080, one node **per zone** |
| `etcd-unreachable` | [etcd cannot be reached](../doc/runbook/membership.md) — one node, cluster of one | `AWSFIS-Run-Network-Blackhole-Port` on 2379, egress |
| `node-latency` | [A node that is up but wrong](../doc/runbook/nodes.md), [threads are all waiting](../doc/runbook/nodes.md) | `AWSFIS-Run-Network-Latency` toward the VPC |
| `disk-fills` | [RocksDB returned an error](../doc/runbook/storage.md), [the disk is filling](../doc/runbook/storage.md) | `AWSFIS-Run-Disk-Fill` |
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

Roughly twenty minutes for the agentless three, most of it `node-stops` waiting for the auto
scaling group to launch a replacement and for that replacement to rebuild itself. The faults
themselves are minutes; the waiting is the deployment's own timings — a ten second lease, a
sixty second load balancer health check, a two hundred second grace period — and
`CHAOS_SETTLE` and `CHAOS_RECOVERY` are how much of each is allowed for.

## The role, and the permission to use it

`chaos.json` is a stack of its own holding two things, and it is separate from
`cloudformation.json` deliberately: neither of them is a thing to leave standing beside a
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

## The faults that need the agent

Four of the seven inject their fault with `aws:ssm:send-command` and one of the `AWSFIS-Run-*`
documents, and **they are skipped unless `CHAOS_SSM=1`**:

```bash
CHAOS_SSM=1 chaos/run.sh
```

The reason is [the network](../doc/deployment/network.md). Those documents install what they
need — `atd` for the rollback timer, `tc` for the latency — from the distribution's own
repositories, and **no instance in this stack has a route to the internet**. Where a NAT gateway
would be there are seven VPC endpoints, and none of them is `cdn.amazonlinux.com`. So on the
stack as it stands the SSM faults will fail at their precondition, and FIS reports that as an
experiment that `failed` with a reason, which the harness prints.

Three ways to have them, in the order they are worth considering:

1. **Bake the dependencies in.** One `dnf -y install at iproute-tc` in the database tier's user
   data, and every one of them works. It costs a package install on every boot, inside the
   [two hundred second grace period](../doc/runbook/deployment.md), and that budget is not
   generous already.
2. **Run them against a stack with a route out**, which is what a `NatGateway` parameter would
   be for.
3. **Leave them skipped**, which is what the pipeline does. The four failure modes they cover
   are the four that are least dangerous to be wrong about: three of them are read-only
   degradations, and the fourth is the disk filling.

The agentless three need none of this, because the fault is a network access control list or an
instance state that the service changes from outside. That is the whole reason they are the
default.

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

One thing it covers and reports rather than asserts: **the runbook overstates what survives**
[a node that does not answer in every zone](../doc/runbook/nodes.md). It says reads and writes of
individual keys are fine there, and for reads that is exactly true — but a write needs *every*
copy, so roughly seven writes in eight touch one of the three deaf nodes and are refused.
`scan-loses-a-node` prints that number rather than asserting on it.

## Everything is an environment variable

As in [`perf/`](../perf).

| Variable | Is | Default |
| --- | --- | --- |
| `CHAOS_EXPERIMENTS` | Which experiments, in what order | all seven |
| `CHAOS_SSM` | `1` runs the four that need the agent | unset, so they are skipped |
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
