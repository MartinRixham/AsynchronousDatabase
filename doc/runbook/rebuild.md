# Rebuilding a node

A replaced instance is an empty database: its root volume went with the instance,
and nothing else in the cluster puts that copy back — there is no read repair, no
anti-entropy, no hinted handoff and no replication log.

**A node fills itself in on the way up.** Before it registers in etcd, an
instance that finds its store empty reads a zone that still holds its records and
writes back the ones it is about to own. No operator step, no script.

## Before it joins, on purpose

The ordering is the whole design, and it is worth being explicit about why.

```
serve()
  ├── discover()   read the membership from etcd, join nothing
  ├── rebuild()    fill the store from another zone      ← not a member yet
  ├── start()      register in etcd, renew the lease
  └── accept()     start answering
```

A node that has not registered is **nobody's copy**. No read is answered from it,
no write is waiting on it, and the keys it is about to own are still held and
served by the nodes that own them now. So the rebuild can take as long as it
takes and cost the cluster nothing.

Registering first and rebuilding afterwards would be the opposite. **Every copy
has to take a write**, so a node that is in the membership but not yet ready is a
node that every write to its partitions fails on. Rebuilding before joining turns
what would be a write outage into a slower start-up.

It is also why the port is not open yet. A node that accepted requests before
registering would see a membership of one, decide it owned every key, and answer
`404` for all of them — [the cluster-of-one failure](/runbook/membership#etcd-cannot-be-reached)
on purpose. Failing the health check while it works is the correct answer.

The socket is *bound* from the start, because binding is what settles the port,
but it does not **listen** until the rebuild is done. That distinction is the
whole of it: a bound socket that is not listening refuses a connection at once,
where a listening one that nothing is accepting on takes the connection and then
answers nothing — so a health check or a neighbour would wait out its own
timeout rather than being told to go somewhere else.

### Why a cluster starting together does not wait for itself

A node blocking its own start-up on its neighbours answering is a cluster that
can deadlock: every node waiting for every other, and none of them listening yet.
What stops it is that **a node writes itself into etcd only once it is ready, so
the neighbours a rebuild finds there are neighbours that have finished their
own.**

It is worth being exact about what that does *not* say. The membership a rebuild
reads is not one this node is absent from: `read_members()` puts this node back
whatever etcd says, which is what makes "will I own this key?" answerable before
joining. What is absent from it is every *other* node that has not registered
yet — and that is the part the argument rests on.

So a cluster coming up together finds fewer than two members, has no second zone
to read, and starts. A single replaced node coming up beside neighbours that are
already serving sees them, and they answer. The nodes a rebuild waits on are
exactly the nodes that have already finished their own.

A membership handed to the server rather than discovered — which is what a test
does — is not one a rebuild acts on, for the same reason: nothing in it says
which of those nodes are listening yet.

The case this leaves is narrow and worth knowing: **every node restarted within
one lease, with every store wiped.** The registrations of the run before have not
expired yet, so each node reads neighbours that are not listening, and spends one
timeout on each before giving up and starting empty. There is nothing to rebuild
in that state anyway — every store is gone — so the cost is a slow start-up and
not a wrong one.

## When it runs

All three have to be true, or the node starts as it always did:

| | Or else |
| --- | --- |
| The store holds **no tables at all** | A node that kept its store keeps it. Reading a whole zone on every ordinary restart would cost the keyspace to learn that nothing is missing |
| The instance is **clustered** | `ASYNCDB_ETCD` and `ASYNCDB_NODE` are set, so there is an etcd to read a membership from |
| There is **more than one zone** | What this node is missing is missing from its own zone as a whole, so the copy to read has to be in another one |

So a container that restarted, or a host that rebooted, rebuilds nothing — the
volume is still there and the store opens as it was. A replaced instance, a fresh
volume, or a `docker compose down -v` is what triggers it.

## What it does

1. Reads the membership from etcd **without registering**. A node is a member of
   its own cluster whatever etcd says, so the copies of a key are already the
   copies it will have once it joins — which is what makes "do I own this?"
   answerable before joining. Nothing is rebuilt from a membership that was not
   read here.
2. Takes the zones in order and picks the first that is not its own.
3. Asks one node of that zone for the tables, and writes them straight into the
   store. The schema is *written*, not declared: the graph was validated when it
   was created, so nothing has to name its dependencies in order.
4. Asks **every** node of that zone for
   [a file of its own share of each table](/database/cluster#moving-a-share-of-a-table),
   naming the partitions this node is about to hold, and takes each file into the
   store as it arrives. Each table is read in
   [four pieces at once](/database/cluster#several-pieces-at-once), and the files
   of the next round are asked for while the last round's are still going in.
5. Registers, and starts serving.

Two details of the API carry it:

| Used | Because |
| --- | --- |
| A file rather than a scan | A scan pages a hundred records at a time, so a node holding hundreds of gigabytes would need millions of round trips to be filled. A file is one round trip for as much of the table as the budget covers, and the transfer then waits on the bandwidth between the two nodes rather than on the time to ask |
| Several pieces of the table at once | One walk at a time is *ask, wait, take it in, ask again*, with the node being read, the network and the store each idle for most of it. The pieces are read together, and the next file of each is asked for while the last is still going in |
| The partitions named by the node asking, not the node answering | The node serving a file asks its own membership nothing: it filters by the key's partition, which is a function of the key's partition key alone. So a source a moment behind in what it thinks the cluster is still sends the right records |

A zone with a node that does not answer is a zone that cannot give the whole of
what it holds, so the **next zone is asked for the whole thing again** — the same
fallback a scan already makes. If no zone answers, the node starts with what it
has, which is what it would have had anyway.

**It is bounded by progress, not by a clock.** A rebuild goes on for as long as
files keep arriving; what gives up is ten minutes of being answered *nothing*.
A wall clock over the whole of it is a clock that a large enough store always runs
out — how long a rebuild takes is how much there is to read, and a share of a
terabyte and a share of a megabyte are the same code. The node is not in the
membership while this runs, so a slow one costs a slow start and nothing else.

Every round trip inside has a timeout of its own; this is the bound on a node that
has stopped answering between them. A rebuild that gives up stops at a file
boundary and the node starts thin, which is a copy the cluster has rather than one
it is still waiting for — so what is lost is the file it did not ask for and never
a file half taken. It will not try again: an empty store is the only trigger, and
the store is no longer empty.

## A node that came up short

A rebuild that did not read the whole of this node's share leaves the node
holding less than it owns, and **the node knows it**. That matters because of
what a miss means: a key this node has nothing for is a key that was never
written *or* one it never received, and a node that cannot tell them apart must
not answer as though it could.

So a node in that state answers `503 node_incomplete` where it would otherwise
say the key or the table is missing, and the node that asked
[tries the next copy](/database/cluster#what-a-write-and-a-read-do) instead of believing
it. A `404` is a claim about the keyspace; `node_incomplete` is a node declining
to make one.

It is not a node out of service. It still serves every key it does hold, still
takes writes, still answers `/health` with `200` — so the load balancer keeps it
— and `"incomplete": true` is where it shows:

```bash
curl -s http://asyncdb-3:8080/health | jq '.incomplete'
```

**What clears it is a reconcile pass that settles**, which is this node having
fetched everything it owns and holds nothing for. A pass runs when the membership
moves, so a node that came up short and then sees no membership change at all
stays short — the rebuild will not run again, because an empty store is its only
trigger and the store is no longer empty. Declaring the tables again, or any
change that moves the membership, is what starts the pass that fills it.

Nothing the rebuild does is worth dying over either. A store that refuses a
write, or a neighbour that answers something unreadable, is logged and the node
starts — because a process that fell over here would fall over in the same place
when it was restarted, and never register at all.

A file is held whole in memory at both ends, which is what sizes the budget: one
walk is 64 MiB of the table, shared out between the pieces it is read in, and a
table this node holds a fraction of is a file that fraction of the size. So a
rebuild in four pieces costs what a rebuild in one piece does.

## Watching it

It is on the `DEBUG` log, which the image has on:

```bash
docker logs asyncdb-3 2>&1 | grep -i rebuil
```

```
DEBUG: Rebuilt 8204 records before joining.
```

From outside, a node that is rebuilding is a node that is not yet answering: it
is absent from every other node's `/health`, and its own port is not open. When
it appears in the membership it has either read the whole of its share or is
saying it did not, in its own `incomplete`.

```bash
curl -s http://asyncdb-1:8080/health | jq '.nodes'
```

## What it does not do

- **It does not repair a node that is merely thin.** The trigger is an empty
  store. A node that lost one record to a write that one copy refused holds
  tables, so it rebuilds nothing — that record comes back when it is written
  again, and nothing else brings it back. A node that lost a record that way is
  not `incomplete` either: what that flag reports is a rebuild that came up
  short, and nothing here detects a gap by looking for one.
- **It is not continuous.** One pass, on the way up. A copy that falls behind
  afterwards stays behind.
- **It is not a backup.** It needs a zone that still holds the data. Lose every
  zone's copy of a partition and there is nothing to read from.
- **It is not what moves records when the membership changes.** That is
  [the pass below](#when-ownership-moves), which runs while the node is serving
  and on every membership change rather than once on the way up.
- **It is not free.** It reads every key and value this node will own across a
  zone boundary, and the node is not serving while it does. On a large store that
  is a slower start-up, and the load balancer will hold traffic off until it is
  done.
- **It is not as parallel as the cluster is.** One node reads from one node at a
  time, in four pieces. What it is *not* doing is reading from every node of the
  zone at once, so a share spread over eight nodes is eight walks one after
  another.

## The one hazard

**A table delete that lands during a rebuild is taken back by the next pass, not
lost.** A rebuild reads the schema before it reads a record, and the node running
one is not yet in the membership, so a delete carried to every node in that
window is not carried to it: it comes up holding a table the cluster dropped.
What corrects it is the [tombstone](/database/cluster#the-schema-is-one-record-and-every-name-in-it-carries-a-version) —
the name stays in every other node's schema, stamped at the version the delete
was ordered in, and the first reconcile pass that reads a peer's schema sees the
table held live at an earlier version and drops it.

Records cannot come back at all: nothing erases one but dropping its table.

**What no pass can see is a table dropped and created again while a node was away
for both.** Its column family holds the first incarnation's records; the schema
it reads names the table live at a later version, and a live entry never drops a
column family — deliberately, because the same thing happens when one create is
stamped twice, and dropping on that would take the records of a table the cluster
still has. So the node keeps the old records under the new table. It is narrow —
both operations inside one absence — and the remedy is the delete run once more
with the node in the membership. It is the same reason the docs say to grow a
cluster at a quiet moment.

## When ownership moves

A rebuild fills a node that has nothing. The other half of the same problem is a
node that has the wrong things, and that is a **reconcile**: a pass on a thread of
its own, watching the membership, that runs when it moves.

A key belongs to one node in each zone, and which node that is falls out of the
membership. So a node joining or leaving redraws the split inside its zone
without moving a single record, and the pass is what moves them:

| | Is | Why it matters |
| --- | --- | --- |
| **Fetch** | Records this node now owns and holds nothing for, taken from a node that has them | Until it does, its zone is a copy short: the record is answered only by the zones that happen still to have it |
| **Clear down** | Records this node no longer owns, deleted once the node that owns them now has them | Nothing reads that copy and it stops taking writes, so it comes back as a **stale answer** if the membership ever hands the partition back |

**Neither half is safe without the other**, which is why one pass does both.
Clearing down alone is a shrink that loses records rather than staling them.
Fetching alone is a store that only grows and a stale value waiting for the next
membership change.

**The schema comes first.** A record can only be written where its table is, so a
pass reads the schema from a node that has it before it asks for any records, and
takes every name that node holds at a later version than this one does. It is the
only thing that puts a table on a node that missed the create — the rebuild
copies them too, but the rebuild runs on an empty store alone.

**What drops a table here is a tombstone and nothing else.** A name the peer says
nothing at all about is a node that is wrong about the schema rather than a
delete this node missed, and dropping a table takes its records with it, so it is
left standing. A name the peer holds as a tombstone stamped after the create this
node holds *is* the delete this node missed, and that one goes.

**Both halves ask for a file, and neither asks about a record at a time.** The
fetch asks each node for
[a file of the partitions this node now holds](/database/cluster#moving-a-share-of-a-table),
so the only records that cross the network are the ones being taken over.

Both are read in [several pieces at once](/database/cluster#several-pieces-at-once),
the same as a rebuild.

The clear down is two walks. The first is **local**: it reads this node's own keys
and works out which partitions it is holding records it no longer owns in, grouped
by the node of its own zone that owns them. The second asks each of those nodes
for a `values=false` file of the keys it holds in exactly those partitions, and
every key in both stores is a copy this node may give up. One question a share,
where asking about a record at a time was a round trip for every record a
membership change moved.

### What makes deleting safe

A copy is given up only when **the node that owns the key in this node's own zone
answers with that key**. Not any copy: the record here is this zone's copy of it,
so deleting it because another zone still has one is a zone left holding nothing.

Asking about a share rather than a record does not weaken that. The file names
keys, and a key it does not name is a key that is kept — so a node that has not
fetched anything yet answers a file without those keys in it, and nothing is
deleted. That is also what makes a wrong membership harmless: a node acting on a
view that is a moment out of date keeps the record and asks again on the next
pass. What is left over when a pass is done is its `deferred` count.

### What it does not do

- **It is not a repair.** It moves what some node still has. A key whose owner in
  every zone was terminated by the same update is gone, and this does not bring
  it back.
- **It chooses between two values by which was written later, and by nothing
  else.** Every record carries the version the leader stamped it with — a term
  that rises whenever leadership moves, and a count that rises within one — so a
  file replaces a record written before it and leaves one written after it alone.
  There is no clock in this and no guess: a pair it cannot order is two copies of
  one write, and what it does about one of those is keep what it holds.
- **It is triggered by the membership, and by starting on a store this node did
  not fill.** A store that matches the membership it was left with is never
  walked, so a node whose cluster does not change never runs a pass at all. A
  process that comes back to a store it was left with is the exception, and it
  cannot be read off the membership: the share it owns moved to another node while
  it was away and the records it no longer owns are still here, but the membership
  it joins looks like the one it left. A start-up that ran no rebuild — and the
  rebuild runs on an empty store alone — therefore runs a pass of its own. A
  [lease that lapsed and came back](/runbook/membership#etcd-cannot-be-reached)
  under a process that stayed up needs none: that node watched its own membership
  fall to one and rise again, which is two changes.
- **It is one tick behind, deliberately.** The membership is a moment; a pass runs
  on the tick after the change, not on the reading of it.
- **It is not bounded by a count of tries.** A membership change buys twelve passes
  of getting nowhere, and a pass that moved records buys all twelve back: how many
  passes a share takes is how large the share is. Each half of a pass runs while
  files keep arriving and stops when they stop, because a pass cut off part way is
  one the pass after it starts again from the beginning. A node being shut down
  tells the pass in flight to stop rather than waiting it out.

### Watching it

It is on the `DEBUG` log, which the image has on:

```bash
docker logs asyncdb-1 2>&1 | grep -i reconcil
```

> `Reconciled 41 records fetched, 63 cleared, 0 deferred.`

**`deferred` is the number to read.** It counts records this node kept because
the node that owns them has not fetched them yet, and it is what makes the pass
run again. A count that stays above zero for longer than a minute or two is a
node whose peer is not running its own pass — check that peer's membership before
anything else.

**A pass is bounded by its own clock, and each half has half of it.** The fetch
walks every node of every zone, so a store of large values is one it does not
reach the end of — and the clear down behind it runs on a budget the fetch cannot
spend. A pass its clock ended is never settled, however little it found to do, so
it runs again: a line reporting nothing cleared is not a pass that had nothing to
clear.

`chaos/nodes-added`, `chaos/nodes-removed` and `chaos/zone-retired` are the test
of all of this: each moves the tier's shape by a stack update and then asks every
node what is in its store, asserting that every zone holds the same keys and that
no key is held by two nodes of one zone.
