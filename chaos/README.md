# Chaos

The failure modes in [`doc/runbook`](../doc/runbook), injected into the deployed stack with the
AWS CLI and asserted on from outside. The runbook says what each fault looks like, what recovers
by itself and what needs a hand; this is what checks that it still does.

It is a test suite and not a demonstration. Every experiment ends by asserting that what it
broke came back, and an assertion that did not hold is a non-zero exit — which is what lets
`build.yaml` run it last against the deployed stack and fail the build on it.

## The experiments

| Experiment | The failure mode | The fault |
| --- | --- | --- |
| `node-stops` | [A node does not answer](../doc/runbook/nodes.md), [an instance was replaced](../doc/runbook/nodes.md), [the rebuild](../doc/runbook/rebuild.md) | `ec2:StopInstances`, one database node |
| `zone-lost` | [A read needs one copy](../doc/runbook/index.md), [fewer zones than the deployment has](../doc/runbook/membership.md) | A network acl on the zone's subnet, denying the other zones' subnets |
| `scan-loses-a-node` | [A scan fails while everything else works](../doc/runbook/nodes.md) | A `DOCKER-USER` rule rejecting what arrives for port 8080, one node **per zone** |
| `etcd-unreachable` | [etcd cannot be reached](../doc/runbook/membership.md) — one node, cluster of one | A `DOCKER-USER` rule rejecting what the container sends to port 2379 |
| `node-latency` | [A node that is up but wrong](../doc/runbook/nodes.md), [threads are all waiting](../doc/runbook/nodes.md) | A `netem` qdisc delaying everything the host sends into the VPC |
| `disk-fills` | [RocksDB returned an error](../doc/runbook/storage.md), [the disk is filling](../doc/runbook/storage.md) | `fallocate` over what is left of the root volume |
| `etcd-quorum-lost` | [etcd has lost quorum](../doc/runbook/membership.md) | `ec2:StopInstances`, two of the three members |

The order is the order they run in, and it is not arbitrary. The three that need nothing of the
instances themselves come first, because they run against a stack whose agent answers nobody.
`etcd-quorum-lost` is last because it is the only one that leaves the cluster having been *wrong
about itself* rather than merely short of a node, and the pipeline deletes the stack next.

`node-stops` is the one to run if only one is run. It is the only test anywhere of
[the rebuild](../doc/runbook/rebuild.md), and the rebuild is the only thing in the system that
puts a lost copy back.

## Run it

The stack has to be up, and the suite refuses to start against one that is not already whole —
six nodes in three zones, nothing stalled. Chaos against a cluster that is already broken proves
nothing.

```bash
make create-stack                    # or against the stack a build stood up
make create-chaos-stack              # the permission to inject a fault
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

## The permission to break things

`chaos.yaml` is a stack of its own holding one managed policy, and it is separate from
`cloudformation.yaml` deliberately: neither of them is a thing to leave standing beside a
database, and this way both are created for a run and deleted after it.

`ChaosPolicy` is what the suite injects with — stopping and starting an instance tagged `asyncdb`
or `etcd`, writing a network acl, sending a Run Command, and suspending the etcd group's
`ReplaceUnhealthy`. It attaches to the IAM groups named by the `Operators` parameter, which
defaults to `builders`, the group holding the identity the pipeline runs as. Nothing else about
the account grants the destructive half of that, which is deliberate: **outside a chaos run,
nobody in this account can stop an instance of either tier or run a shell command on one.**

Only the stop and start are scoped by tag. The rest is `Resource: "*"`, because the stack
standing at all is the grant, and it stands for a run.

## Validate before you run

```bash
chaos/validate.sh
```

Every experiment's `preflight`, and no fault at all. A preflight resolves the instances, subnets
and groups the experiment would break, dry runs the calls EC2 offers a dry run of, and asks
Systems Manager whether the agent answers on the instances the fault would go through. It is the
whole of "would this experiment run?" for a handful of API calls each, nothing applied, and
seconds — where finding the same mistake by running the suite is the length of the experiment
that hits it. It needs the stack up, because what a preflight resolves is real resources.

It is worth running after any change to an experiment. A chaos stack that is not standing, an
instance whose SSM agent never registered, and a group whose logical id moved are all caught in
seconds here rather than by a full run.

## The faults that go in through SSM

Four of the seven carry their fault onto the instance with `ssm:SendCommand` and the
`AWS-RunShellScript` document, and they run by default like the other three. `node-latency`
installs `tc` from the distribution's own repositories, which
[the route out](../doc/deployment/network.md#the-route-out) is what makes reachable: an
egress-only internet gateway is a route to the internet that opens outwards only and over IPv6
only, so `dnf` reaches the Amazon Linux repositories and the SSM agent reaches Systems Manager
over its dual-stack endpoint.

The cost of that is paid in the deployment rather than here: **that one fault depends on an
instance being able to install a package while it is under test**, and a repository that does not
answer is an experiment that fails at its precondition rather than an assertion that did not
hold. It fails there because `fault_await` waits for every invocation to be running the script
before a single assertion is made — a command the service accepted is not a fault that landed.

The other three need nothing of the instances at all, because the fault is a network acl or an
instance state and both are written from outside. That is why they are still first.

### The shape of an SSM fault

Every one of the four sends the same script, built by `fault_script` in
[`harness.sh`](harness.sh): write the removal down, arm a detached timer, install the fault, sleep,
remove it. **Nothing here waits that sleep out.** `fault_stop` takes the fault away over a second
Run Command as soon as the assertions are done, because waiting is every experiment after this one
measuring this fault instead of its own. The sleep and the timer are for the run that died holding
the fault, they are minutes long, and removing a fault that is already gone is nothing.

The timer is detached with `setsid` rather than left to a trap, because there is no signal to trap:
a Run Command that is cancelled runs no trap in the script it cancelled, and an instance that stops
being asked anything is an instance still holding whatever was installed on it.

### The deaf node is a rule of our own

`scan-loses-a-node` and `etcd-unreachable` install a rule in `DOCKER-USER`, and it has to be that
chain: asyncdb is a container behind a published port, so everything a peer sends it is translated
and forwarded and goes through `FORWARD` and never `INPUT`, and everything the container sends is
forwarded too and never `OUTPUT`. **A rule in `INPUT` or `OUTPUT` blocks nothing here** — the node
would answer its peers throughout and every assertion would be made against a cluster in which
nothing had happened. `DOCKER-USER` is the chain docker leaves in `FORWARD` for exactly this.

The rule **rejects** rather than drops. A node waits thirty seconds on another node, so a dropped
packet is a node that hangs and a reset is a node that does not answer, which is what these two
are about — the copy that does answer is asked next, and `node-latency` is the experiment about
waiting.

`node-latency` needs none of that chain, because `tc` shapes the host's own device and a
container's traffic leaves through it like anything else. Its qdisc goes on whatever the default
route names, which is not `eth0` on an instance of this generation.

## What this does not cover

Not everything in the runbook is a fault an infrastructure service can inject, and the ones that
are not are worth naming so that nobody looks here for them:

| Not covered | Why |
| --- | --- |
| [`Corruption` in RocksDB](../doc/runbook/storage.md) | Damaging a live SST under RocksDB is not a fault, it is a forgery, and what it proved would be about the bytes chosen |
| [The store will not open](../doc/runbook/storage.md) | Two processes over one directory is a host-local lock conflict, not an infrastructure fault |
| [A node in no zone](../doc/runbook/membership.md) | A metadata read that failed at boot. A configuration fault |
| [`invalid_cursor`, `stale_leader`, `table_not_found`](../doc/runbook/errors.md) | Client-level. That is what [`api/`](../api) asserts |
| [A delete during a rebuild](../doc/runbook/rebuild.md) | Terminating the instance is easy; the racing delete needs a harness timed against it |
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

The scan is the same arithmetic from the other side. A node's own zone never includes itself, so a
deaf node answers a scan out of its own store and the one node of its zone it can still reach —
the rule is on what arrives, and nothing stops a deaf node asking. Half the nodes are deaf, so a
scan through the load balancer is a coin toss, and the experiment asks a node that hears over Run
Command instead.

## Everything is an environment variable

As in [`perf/`](../perf).

| Variable | Is | Default |
| --- | --- | --- |
| `CHAOS_EXPERIMENTS` | Which experiments, in what order | all seven |
| `CHAOS_STACK` | The stack under test | `asyncdb` |
| `CHAOS_URL` | The address to drive, instead of the stack's `Url` output | |
| `CHAOS_TABLE` | The table the suite seeds and reads | `chaos` |
| `CHAOS_RECORDS` | How many records it seeds | 200 |
| `CHAOS_SETTLE` | How long a membership change is given | 150 seconds |
| `CHAOS_RECOVERY` | How long an instance replacement is given | 900 seconds |
| `CHAOS_ONSET` | How long a started fault is given to bite | 20 seconds |

Each experiment has one or two of its own — the length of its fault, the size of its latency —
named at the top of the script that uses it.

**A duration is a ceiling and nothing else.** A fault lasts until `fault_stop` takes it away,
which is as soon as that experiment's assertions are done; the seconds an experiment names are
what its script sleeps for if nothing ever comes back to remove it. That is what lets them stay
generous — **a fault that expires mid-assertion is a *false failure* and not a weaker test**,
because `scan-loses-a-node` asserts that a scan **fails** while a node is deaf and goes red if the
node comes back early. Shortening one buys nothing: a fault costs no more for being allowed to
last longer than the assertions take.

Every fault removes itself, and none of it is taken on trust: the recovery assertion that every
experiment runs immediately afterwards is the check that it went, **and it has to be an assertion
the fault would fail**. `scan-loses-a-node` waits on a **write**, not on the membership: the
membership is the one thing that fault never changes, and a recovery check that cannot fail is how
three deaf nodes survive into the next experiment.

`heal` runs from the exit trap as well as from the experiment, so a run that is killed holding a
fault still takes it away — and every one of them is written to be safe run twice, or against a
fault that never landed.

## Reading a run

```
== A node is stopped
   Reads survive it, writes recover with the membership, the replacement rebuilds.

  PASS the stack is running six database nodes
  Stopping i-0a1b2c3d4e5f60718.
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
