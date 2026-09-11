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
| `containers-restart` | [The container has stopped](../doc/runbook/nodes.md), [what recovers by itself](../doc/runbook/index.md) | `SIGKILL` to every container's own process at once, three times over |
| `nodes-added` | [Growing a cluster](../doc/runbook/storage.md), [the rebuild](../doc/runbook/rebuild.md) | A stack update taking the tier to **nine** instances, and back to six |
| `nodes-removed` | [No rebalancing](../doc/runbook/storage.md) | A stack update taking the tier to **three** instances, and back to six |
| `zone-retired` | [Fewer zones than the deployment has](../doc/runbook/membership.md), [the rebuild](../doc/runbook/rebuild.md) | A stack update giving the group **two** subnets instead of three, and `ec2:StopInstances` on what is left in the third |

The last three [assert the two invariants a resize has to leave behind](#the-two-invariants-they-assert),
which is what the cluster's [reconcile](../doc/runbook/rebuild.md#when-ownership-moves) exists to
hold.
| `etcd-quorum-lost` | [etcd has lost quorum](../doc/runbook/membership.md) | `ec2:StopInstances`, two of the three members |

The order is the order they run in, and it is not arbitrary. The three that need nothing of the
instances themselves come first, because they run against a stack whose agent answers nobody. The
five that go in through the agent follow. The
three that resize the tier come after every fault that only breaks it, because they are the only
ones that change what the deployment *is*: a run that dies inside one leaves a stack of a different
shape rather than a cluster short of a node. `etcd-quorum-lost` is last because it is the only one
that leaves the cluster having been *wrong about itself* rather than merely short of a node, and
the pipeline deletes the stack next.

`node-stops` is the one to run if only one is run. It is the only test anywhere of
[the rebuild](../doc/runbook/rebuild.md) a *replacement* runs, and the rebuild is the only thing in
the system that puts a lost copy back. `nodes-added` and `zone-retired` reach the same mechanism
from the other side — a node joining a tier that grew rather than one replacing a tier that lost an
instance — and `zone-retired` is the only one that asks a whole zone's copy to be built.

## Every experiment runs under load

**A fault that lands on an idle cluster is not the fault anybody has.** So the harness keeps a
client on the load balancer for the whole of every experiment — reads as fast as one connection
answers them, and a write every `CHAOS_LOAD_PAUSE` seconds — and the node that is stopped, cut off,
slowed or killed is one that was serving when it went.

The two halves are not there for the same reason.

| | Is | And |
| --- | --- | --- |
| The **reads** | Of the seeded keys, every one of which exists, so a read that is not answered 2xx is the fault and never the key | Reported per phase and never asserted on. How many reads a fault costs is the load balancer's health check interval as much as it is the database |
| The **writes** | Keys of their own, each written once and never again | **The assertion no error code can make**: a write answered 2xx was taken by every copy of the key, so every one of them has to still be there when the fault is over |

A `---- while the node was going away: 121 of 190 reads and 24 of 48 writes were answered 2xx` line
is one phase of one experiment. Every experiment reports at least two: what the load saw while the
fault stood, and what it saw while the cluster recovered.

`expect_load_kept` is the assertion, and every fault that breaks nothing permanently makes it. The
three that **terminate instances** call `report_load_kept` instead and print the number: a key whose
owner in every zone went in the same update went with them, exactly as
[the seed does](#what-is-measured-and-never-asserted).

### Why the load has a table of its own

It writes into `chaos-load` rather than into the seeded table, and that is not tidiness. A write is
answered only once every copy has taken it — but a write that is **refused** may still have been
taken by one of them, because the copies of a write are written beside each other rather than in
turn. Nothing in the cluster puts the rest of that record back: there is no read repair, no
anti-entropy, and a reconcile pass moves the records whose owner moved.

So a load running into the seeded table would leave the zones holding different keys, which is
exactly what [the resizes assert they do not](#the-two-invariants-they-assert). Measured on a two
zone cluster killed twice over: twenty-seven keys apart, every one of them a write the client was
told had failed, and no fewer three minutes later. That is
[a copy that missed a write](../doc/runbook/index.md#what-recovers-by-itself) not recovering by
itself, which the runbook already says — so it is measured, in the `of N writes that were refused, M
are readable anyway` line, and the invariants are left asking about a table whose every write was
acknowledged.

`CHAOS_LOAD=0` turns the whole of it off, which is how an experiment is read against an idle
cluster.

## Run it

The stack has to be up, and the suite refuses to start against one that is not already whole —
six nodes in three zones, nothing stalled, and **every node holding what it owns**. Chaos against a
cluster that is already broken proves nothing.

The last of those is asked of each node rather than of the load balancer. `incomplete` is a node's
own state — [a rebuild that came up short](../doc/runbook/rebuild.md) — and a node in that state
serves what it has and answers `/health` like any other, so a sampled check is one that may never
land on it. Every node is asked over Run Command, and a node that cannot be asked fails the check
rather than passing it silently.

**The same check runs after every experiment**, beside the one that says the cluster came back to
six nodes in three zones. A node that came back from a rebuild holding less than it owns is in the
membership and answering, so the shape cannot show it — and every experiment after it would be
measuring a copy that is short. The run stops there and says which node.

```bash
make create-stack                    # or against the stack a build stood up
make create-chaos-stack              # the permission to inject a fault
chaos/run.sh
make delete-chaos-stack
```

A subset, in the order given — which is also how the pipeline shares the suite between stacks:

```bash
CHAOS_EXPERIMENTS='zone-lost node-stops' chaos/run.sh
```

Against a stack that is not `asyncdb`:

```bash
CHAOS_STACK=asyncdb-two CHAOS_EXPERIMENTS=nodes-added chaos/run.sh
```

One experiment on its own, which is how to read one while it runs:

```bash
chaos/node-stops.sh
```

### What it costs

**About fifty minutes for all eleven, and the resizes are twenty of it.** Measured:

| | |
| --- | --- |
| `nodes-added` | 9.5 min |
| `zone-retired` | 6 min |
| `nodes-removed` | 5 min |
| `containers-restart` | three kills of six containers, a settle between them and the passes after the last — **not yet measured against a deployed stack**, and near eight minutes by the timings around it |
| the other seven, between them | 20 min |

`nodes-added` is the longest because nine instances are three launches and a rebuild apiece, and
there is no way to ask for them sooner.

The [load](#every-experiment-runs-under-load) adds well under a minute to each of them, nearly all
of it at the end: the writes it made are read back in one curl over one connection rather than a
request a process, so thousands of keys are seconds rather than the minutes a loop would take.

The three resizes are all waiting: each is two stack updates, and an update that adds instances is
a launch, a pull and a rebuild before the membership says anything has happened. A replacement is
**a minute and a bit** of that, measured by `node-stops`, so what an experiment costs is the number
of instances it waits for and not their size. Nothing here waits on an auto scaling group's own
rebalancing: `zone-retired` [empties the zone rather than waiting to be rebalanced out of
it](#the-faults-that-are-a-stack-update), which is a quarter of an hour of the scheduler's pacing
that says nothing about this system.

That arithmetic is why [the pipeline runs four stacks at once](../doc/pipeline/index.md#the-shares)
rather than one: nothing here is parallel on a single stack, because an experiment has the cluster
to itself by design. Standing a cluster up and tearing it down is eight minutes a share whatever it
then runs, so the eleven are spread to land the four shares within a minute or two of each other
rather than to fill three of them and leave a fourth long.

They also cost money for as long as they run: `nodes-added` is nine database instances rather than
six for the length of it. Nothing is left behind — every one of them puts the shape back, and the
suite stops if it did not. The faults themselves are minutes; the waiting is the
deployment's own timings — a ten second lease, a load balancer health check of two ten second
intervals either way, a
[thirty second deregistration delay](../doc/deployment/database.md#the-load-balancer), a two
hundred second grace period — and `CHAOS_SETTLE` and `CHAOS_RECOVERY` are how much of each is
allowed for. A node that can order no write takes itself out of the load balancer a lease after
its membership fell short, so the faults that isolate one rather than stop it wait for that as
well: `zone-lost` waits for writes to come back rather than asserting them outright, because
until the check has failed twice the load balancer is still choosing a node that refuses them.

## The permission to break things

`chaos.yaml` is a stack of its own holding one managed policy, and it is separate from
`cloudformation.yaml` deliberately: neither of them is a thing to leave standing beside a
database, and this way both are created for a run and deleted after it.

`ChaosPolicy` is what the suite injects with — stopping and starting an instance tagged `asyncdb`
or `etcd`, writing a network acl, sending a Run Command, updating the stack that is under test, and
suspending the etcd group's `ReplaceUnhealthy`. It attaches to the IAM groups named by the
`Operators` parameter, which
defaults to `builders`, the group holding the identity the pipeline runs as. Nothing else about
the account grants the destructive half of that, which is deliberate: **outside a chaos run,
nobody in this account can stop an instance of either tier or run a shell command on one.**

It reads as well as breaks: the instances, subnets and network acls a fault has to find, and the
scaling activities a failed assertion reports.

Only the stop and start are scoped by tag. The rest is `Resource: "*"`, because the stack
standing at all is the grant, and it stands for a run.

The update is the one that is not destructive on its own, and it is here for the same reason: the
resizes need `UpdateStack` on the stack and `UpdateAutoScalingGroup` on what it changes, and
nothing else in the account grants either. It is not the permission to *deploy* — that is the
pipeline's identity, and standing the stack up is still its job.

## Validate before you run

```bash
chaos/validate.sh
```

Every experiment's `preflight`, and no fault at all. A preflight resolves the instances, subnets
and groups the experiment would break, dry runs the calls EC2 offers a dry run of, asks Systems
Manager whether the agent answers on the instances the fault would go through, and — for the three
that resize the tier — creates the **change set** a stack update would apply, reads what it would
change and deletes it. That last one is how a stack created before the template took `Nodes` and
`Zones` is caught in a second rather than ten minutes into an experiment. It is the
whole of "would this experiment run?" for a handful of API calls each, nothing applied, and
seconds — where finding the same mistake by running the suite is the length of the experiment
that hits it. It needs the stack up, because what a preflight resolves is real resources.

It is worth running after any change to an experiment. A chaos stack that is not standing, an
instance whose SSM agent never registered, and a group whose logical id moved are all caught in
seconds here rather than by a full run.

## The faults that go in through SSM

Five of them carry their fault onto the instance with `ssm:SendCommand` and the
`AWS-RunShellScript` document, and they run by default like the rest. `node-latency`
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

The three resizes are a third case: nothing of theirs is *injected* through the agent, and their
assertions are asked over it — what a resize moved is a question about one node's store, and only
Run Command reaches one node. A run whose agent stops answering fails them on
`every node said what it holds`, which is deliberately an assertion rather than a shrug: a node that
cannot be asked and a node that holds nothing must not read alike.

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

### The kill is a kill, and it comes from the host

`containers-restart` sends no rule anywhere. What it runs on each instance is `kill -9` on the
container's own init, found through `docker inspect`, and then a wait for that node's API port to
answer again. Neither half of that is incidental:

- **From the host, because a container cannot kill itself.** The kernel drops a signal a PID
  namespace sends its own init unless the init handles it, so a `kill -9 1` over `docker exec`
  does nothing at all.
- **`kill -9` and not `docker kill`.** `docker run --restart always` is the whole of the recovery
  here and the experiment asserts it — nothing in the script starts a container — so the exit has
  to be one the daemon reads as the process dying rather than as a stop it was asked for, which it
  may record as manual and not restart. A process killed from the host is the first of those
  whatever the daemon's rule about the second is, and it is also the failure the runbook
  describes: [a container that exited comes back at
  once](../doc/runbook/index.md#what-recovers-by-itself).
- **The wait is on the port and not on `docker ps`.** A node listens only once it has opened the
  store and joined the cluster, so a round that waits for an answer is a round whose next kill
  lands on a node that was serving rather than on one that was still starting.

The kills of one round go out in **one** Run Command rather than six, because six sends in turn
are six kills a wait apart — a rolling restart, which is a fault the cluster is built to ride out
one node at a time and not the one this asserts on. `ssm_all` in [`harness.sh`](harness.sh) is
that send, and it is every instance or none: Systems Manager refuses a batch naming an instance it
does not know, and a kill that quietly skipped a node would be a weaker fault reported as the
whole one.

### The one experiment that is allowed to cost nothing

Every other fault here takes something away and the assertions are about what is left.
`containers-restart` takes nothing: a container is a process, the store is a volume that outlives
it, and a node comes back at the address it already had, owning the partitions it already owned.
So the numbers after the kills have to be the numbers before them, and the experiment is written
as five claims that each say what losing them would mean:

| Asserted | Losing it is |
| --- | --- |
| Every write the cluster acknowledged reads back what was written | A 2xx that was not durable. A write is answered only once every copy has taken it, and every copy of it was then killed with the write ahead log unflushed |
| Every seeded record is still there, at the value written before the kills | A store that did not outlive its container, or one that came back older than it was |
| No copy of a key answers a different value from another copy | Divergence. It is the one question the [holdings](#the-two-invariants-they-assert) cannot put: a key set says which records a zone has, never what is in them |
| Every key the cluster acknowledged is in every zone, and in one store of it | Records moving when no ownership did. The membership came back naming the same six addresses |
| Every partition is led again, and no node holds less than it owns | A cluster that registered without becoming one. Every claim in the cluster is held on a node's own lease, so killing every node frees all 256 |

**A restart is not a rebuild**, and that is asserted too: an empty store is the only thing that
triggers one, so a node whose volume outlived its container reads nothing from anybody. A rebuild
line in `docker logs` since the first kill would mean a node came back to an empty directory,
which is the one way a kill here could cost a copy.

What it deliberately does **not** assert is the resizes' invariant that every zone holds the *same*
keys, and the reason is the writer. A write is answered only once every copy has taken it, but a
write that is **refused** may still have been taken by one of them — the copies are written beside
each other rather than in turn — and killing a leader mid-write is how that happens. Nothing in the
cluster puts the missing copies of such a record back: there is no read repair, no anti-entropy,
and a reconcile pass moves the records whose owner moved, which after this fault is none of them.
That is [a copy that missed a write](../doc/runbook/index.md#what-recovers-by-itself) not
recovering by itself, which the runbook already says. Measured against a two zone cluster killed
twice over: twenty-seven keys apart, every one of them a write the client was told had failed, and
no fewer three minutes later. So the count is printed and the assertion is made over the keys the
cluster acknowledged, which is the claim that holds.

The writer is what the first of those needs and no other experiment has: a client writing
throughout, each key once and never again, recording which writes were acknowledged. A key written
twice could read back either value with nothing wrong, and then the check would say nothing.

## The faults that are a stack update

`nodes-added`, `nodes-removed` and `zone-retired` break no node's ability to answer. The shape of
the database tier is two parameters of `cloudformation.yaml` — `Nodes`, how many instances the
group runs, and `Zones`,
how many subnets it is given — and moving one of them is an `UpdateStack` carrying that parameter,
the deployed template, and every other parameter as it stands. The auto scaling group does the rest:
it balances what it is given over the subnets it spans, and an instance reads its own availability
zone out of IMDS, so nothing is ever *told* what shape the cluster is.

The two parameters are the two factors, one each:

| | Is | Moving it |
| --- | --- | --- |
| `Zones` | How many copies of the keyspace there are — a zone holds exactly one | `zone-retired`, three copies down to two and back |
| `Nodes` | How many ways a zone splits the copy it holds | `nodes-added` to nine, `nodes-removed` to three |

**Three is the ceiling for `Zones` and two the floor**, because there is no fourth subnet to grow
into and one zone is no replication at all. So the increase is asserted on the way back rather than
as a fault of its own — the retirement is the fault, and putting the zone back is a copy that has to
be *built* rather than one that was waiting, which is the half `zone-lost` cannot test.

**`zone-retired` empties the retired zone itself**, and it is the one place a resize touches an
instance. A group given one subnet fewer moves what is in the one it lost when it gets round to it —
a quarter of an hour of a cluster doing nothing, and the scheduler's own pacing rather than anything
here. So the instances outside the subnets the group now spans are stopped as soon as the update
lands, which is [what an operator does to an instance the group will not
act on](../doc/runbook/deployment.md#the-group-does-not-replace-a-failed-application): the health
check is `EC2`, so the group terminates them and launches their replacements in the two subnets it
has left. Both go at once, which is the harder half of it — the zones that stay redraw their split
while the replacements are still booting. What is given up is the one fact that belongs to the
scheduler and not to this system: that the group would have got there unaided.

`heal` is the update back, so a run that dies inside one still leaves the stack the shape it found
it. Starting a stopped instance is best effort beside it, for the case the group has not already
terminated one. The template is `--use-previous-template` throughout: what is under test is the
stack the pipeline stood up, and carrying the checkout's template would be a second change nobody
asked for.

### The two invariants they assert

These hold because the cluster **moves records when ownership moves**, in both directions — the
[reconcile pass](../doc/runbook/rebuild.md#when-ownership-moves) every node runs when the membership
changes. These three experiments are the test of it, and they were written before it existed: each
assertion below names the half of the mechanism whose absence makes it fail.

Every resize is asked the same two things once it has settled, and they come from
[the cluster spec](../doc/database/cluster.md) rather than from what the code does today:

| Invariant | Is | The half that holds it |
| --- | --- | --- |
| **Every zone holds the same keys** | A zone holds a copy of the whole keyspace, so two zones naming different keys is a copy that is short | The **fetch**: a node that gains a partition takes what it now owns from a node that has it |
| **No key is held by two nodes of one zone** | A zone's nodes split the copy it holds, so a key in two of their stores is a node that kept what it stopped owning | The **clear down**: a node that loses a partition gives up what it no longer owns, once the node that owns it has it |

Both are **convergence** assertions, not instant ones. Moving records because ownership moved is
work in the background rather than part of the update that caused it, so each is asked again until
it holds or `CHAOS_CONVERGE` runs out — after the membership has already settled, so the seconds are
the mechanism's own and not the auto scaling group's.

They are two halves of one thing and neither is safe alone. Clearing down without fetching is a
shrink that loses records rather than staling them; fetching without clearing down is the disk never
coming back and a stale value waiting for the membership to swing again. Written as a pair, the
suite says which half is missing: the first assertion fails when nothing fetches, the second when
nothing clears down.

**Neither can be seen through the load balancer.** A read is answered by whichever copy has the key
— the owner, or another zone when the owner holds nothing — so it says a record exists *somewhere*
and never where. The assertions ask each node what is in its own store, over Run Command, with a
scan carrying `X-Asyncdb-Forwarded`: a forwarded request is served where it lands, so the answer is
that node's own share rather than its zone's merged one. It is the request a rebuild makes of each
node of a zone.

`nodes-added` makes the second half visible through the API as well. It writes a value of its own
into every seeded key while the tier is nine wide and reads them back once it is six again, and
asserts that **none of them answers the older value**: a key handed back to a node that stopped
owning it either has to be given it again or was never let go, and a stale answer is the second of
those. The three numbers it prints beside that — fresh, stale, gone — are what the clear down is
worth in a line: before it existed, four keys in sixty came back holding a value that had been
overwritten while the tier was wider.

`nodes-added` also asserts that **a node that joined leads partitions of its own**. Which node leads
a partition is worked out from the membership, but a claim is held on the claiming node's lease and
a membership change costs no node its lease — so a node giving up the claim on what it is no longer
named for is the whole of what leaves anything for a new node to claim. Nothing else here would show
that missing: only a write needs a leader, and a write is ordered by whichever node holds the claim
whether the membership still names it or not. It is asked of the joined nodes over Run Command,
because `leads` is a node's own count and the load balancer answers from whichever node it picked.

### What is measured and never asserted

**What a terminated instance took with it.** Every resize prints how many of the seeded keys are
still held by some node, and how many of the writes the load made and the cluster *acknowledged*
are held by no copy afterwards — the same loss counted over records written while the tier was
moving rather than before it. A key whose owner in *every* zone was terminated in the same update went
with them — every copy of it left at once — and no mechanism inside the cluster puts that back:
there is [no rebuild of a copy](../doc/runbook/storage.md#what-there-is-not) and no backup. Roughly
one key in eight of a six-to-three shrink is in that position, and the number is the deployment's
own hashing rather than anything the database decides.

The shape of a failed read is asserted, though: every read of a resized tier is a 2xx or a 404,
never a 5xx and never a request that did not answer. A key a resize took away is not found; a
cluster that cannot answer is a different fault, and a count of failures alone cannot tell the two
apart.

## What this does not cover

Not everything in the runbook is a fault an infrastructure service can inject, and the ones that
are not are worth naming so that nobody looks here for them:

| Not covered | Why |
| --- | --- |
| [`Corruption` in RocksDB](../doc/runbook/storage.md) | Damaging a live SST under RocksDB is not a fault, it is a forgery, and what it proved would be about the bytes chosen |
| [The store will not open](../doc/runbook/storage.md) | Two processes over one directory is a host-local lock conflict, not an infrastructure fault |
| [A node in no zone](../doc/runbook/membership.md) | A metadata read that failed at boot. A configuration fault |
| [`invalid_cursor`, `stale_leader`, `table_not_found`](../doc/runbook/errors.md) | Client-level. That is what [`api/`](../api) asserts |
| [A table delete during a rebuild](../doc/runbook/rebuild.md) | Terminating the instance is easy; the racing delete needs a harness timed against it |
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
| `CHAOS_EXPERIMENTS` | Which experiments, in what order | all eleven |
| `CHAOS_STACK` | The stack under test | `asyncdb` |
| `CHAOS_URL` | The address to drive, instead of the stack's `Url` output | |
| `CHAOS_TABLE` | The table the suite seeds and reads | `chaos` |
| `CHAOS_RECORDS` | How many records it seeds | 200 |
| `CHAOS_SETTLE` | How long a membership change is given | 150 seconds |
| `CHAOS_RECOVERY` | How long an instance replacement is given | 900 seconds |
| `CHAOS_ONSET` | How long a started fault is given to bite | 20 seconds |
| `CHAOS_CONVERGE` | How long a resized cluster is given to move the records whose owner changed | 300 seconds |
| `CHAOS_LOAD` | 0 for an experiment against an idle cluster | 1 |
| `CHAOS_LOAD_PAUSE` | Seconds between the load's writes | 0.2 |

Each experiment has one or two of its own — the length of its fault, the size of its latency, the
time a resized group is given to reach its new shape — named at the top of the script that uses it.

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

  Seeded 200 records into chaos.
  A client is reading chaos and writing chaos-load throughout.
  PASS the stack is running six database nodes
  Stopping i-0a1b2c3d4e5f60718.
  PASS the fault was injected
  PASS the stopped node left the membership and the zone count did not change
  ---- while the node was going away: 143 of 190 reads and 24 of 48 writes were answered 2xx
  PASS every read is answered with the node out of the membership
  ...
  PASS every write the cluster took once the replacement had joined is still there
  PASS and every one of them reads back what was written
  ---- of 61 writes that were refused, 9 of the first 61 are readable anyway
```

A `PASS`/`FAIL` line is an assertion. A `----` line is something measured and reported and never
asserted on — the reads a client lost while the load balancer had not yet noticed a dead target
are the load balancer's health check interval and not the database, and latency here is
[the same as it is in `perf/`](../perf): reported, never a threshold.

The two lines the [load](#every-experiment-runs-under-load) adds are of both kinds. What it saw in
a phase is measured; that every write the cluster **acknowledged** is still there is asserted, and
it is the one claim here that no error code could have made.

A shape the cluster never reached carries one `----` line more: the last few scaling activities of
both auto scaling groups. **A membership that never arrived and an instance that was never
launched read the same from outside**, and this is what tells them apart — `/health` says the
shape the cluster has and the group says the shape it wants, and a launch the account had no room
for is a `Failed` activity there and nothing at all anywhere else.
