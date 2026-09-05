# The cluster

One instance of asyncdb owns one RocksDB. Several instances own a keyspace
between them: each key lives on one instance **in each zone**, and every instance
answers for every key, by asking an instance that holds it when it does not.

A cluster that is told nothing about zones is every instance in one zone, which
is one copy of a record and the partitioning this has always been. Tell each
instance which zone it is in and the same keyspace is kept once per zone — one
copy of every record in every availability zone.

Instances find each other through **etcd**. There is no leader, no coordinator
and no configuration naming the other nodes: a node registers itself, reads who
else is registered, and computes the same answer as every other node to the only
question the cluster has to agree on — which node owns a key.

```
                      ┌────────┐
                      │  etcd  │   /asyncdb/node/<address> per node, on a lease,
                      └───┬────┘   naming the zone the node is in
           ┌──────────────┼──────────────┐
           │              │              │
      ┌────┴────┐    ┌────┴────┐    ┌────┴────┐
      │ node 1  │    │ node 2  │    │ node 3  │   a write goes to the node that
      │ zone a  │────│ zone b  │────│ zone c  │   holds the key in every zone;
      └─────────┘    └─────────┘    └─────────┘   a read to the nearest of them
        key 4821       key 4821       key 4821
```

## Turning it on

Three environment variables, and nothing else:

| Variable | Is |
| --- | --- |
| `ASYNCDB_ETCD` | Where etcd answers. One base URL, or every member of the etcd cluster separated by commas |
| `ASYNCDB_NODE` | This node as the others reach it: `http://asyncdb-1:8080` |
| `ASYNCDB_ZONE` | The availability zone this node stands in: `eu-west-2a`. Unset is a cluster of one zone, which is one copy of a record |

```
ASYNCDB_ETCD=http://etcd-1:2379,http://etcd-2:2379,http://etcd-3:2379
```

Name all of them. Every member of an etcd cluster answers for the whole of it,
so a node that cannot reach the member it was using asks the next one and stays
with whichever answered. Naming one member makes that member a single point of
failure for the *membership* — records are still served, because a node that
cannot reach etcd carries on as a cluster of one, but nothing learns about
anything joining or leaving until it comes back.

Set neither of the first two and the instance is what it has always been: one
process owning the whole keyspace, talking to nothing. Set both and it joins;
set the third as well and it is a copy of the keyspace in its own zone.

`ASYNCDB_NODE` is the address of the **API port**, not of the nginx in front of
it — nodes talk to each other directly, and they do not go through the `/asyncdb`
prefix the browser uses.

etcd is spoken to over its JSON gateway rather than over gRPC, which is why
there is no gRPC dependency here: a node registers with `POST /v3/kv/put` and
libcurl, the same libcurl the API already uses.

## Membership

A node registers `/asyncdb/node/{address}` with a **lease** and renews it every
few seconds. The value is what the node knows that no other node does — where it
answers and which zone it is in:

```json
{ "node": "http://10.0.1.23:8080", "zone": "eu-west-2b" }
```

A value that is not a document at all is read as an address in no zone, so a
cluster half way through an upgrade still agrees about who is a member.

The lease is what makes membership honest: a node that stops
renewing — because it is dead, or partitioned, or too slow — has its key removed
by etcd, and the other nodes stop sending it keys. A node that shuts down cleanly
revokes its lease and is gone at once rather than at the end of it.

Revoking is best effort, and the lease is the guarantee. Everything is often shut
down together, so a node on its way out may find etcd already gone; it says so
and leaves, and etcd drops it when the lease runs out. It spends one timeout on
this rather than one for every member, because a node being stopped has ten
seconds before it is stopped for good.

A node that cannot reach any member of etcd keeps serving the keys it holds and
answers as a cluster of one. It is the safe way to be wrong: refusing to answer
would turn one broken etcd into a broken database.

Only a member that does not answer, or that answers that it is not serving, is a
reason to try the next one. A member that refuses a request has given the answer
the whole cluster would give — a lease that is not there is not there on any of
them — and asking the rest would only be slower.

## Which node owns a key

Every node scores the key with every member's name, and the highest score wins:

```
owner(key) = the node n maximising hash(n, key)
```

This is **rendezvous hashing**, and it is chosen over dividing the keyspace into
ranges for two reasons:

- **There is nothing to agree on but the membership.** Every node computes the
  owner from the list of names it read from etcd, so two nodes with the same list
  give the same answer without ever talking to each other.
- **A change moves as little as possible.** When a node leaves, its keys are
  spread over the remaining nodes and no other key moves. When one joins, it
  takes a share from each node and nothing else moves.

**The key alone decides — the table does not.** The same key in two tables lives
on the same node, so a record and the records derived from it under the same key
are one hop, not two. That is the shape asyncdb is for: a table and the tables
[derived from it](/database/tables#dependencies).

## Partitions

A key belongs to a **partition** — 256 of them, fixed for the life of a cluster
— and the partition is what a node holds, leads and moves:

```
partition(key) = hash(key) mod 256
```

The number is not configurable, because changing it moves every key.

Partitions exist so that the copies of a key are a *set* rather than a
coincidence. The hash below runs on the partition, not on the key, so every key
of a partition is held by the same three nodes in the same three zones — which is
what makes a partition something a node can lead.

## One copy in every zone

The membership is grouped by zone, and the same hash is run **once inside each
zone**:

```
copies(key) = for each zone z, the node n in z maximising hash(n, partition(key))
```

So a zone holds exactly one copy of a key: one, because a zone's nodes score the
key between themselves and one of them wins it; exactly one, because every zone
runs that on its own. Twelve nodes in three zones of four are three copies of the
keyspace, a third of a copy on each node, and no key on two nodes of one zone.

Deciding zone by zone is what makes a zone survivable rather than expensive:

- **A node leaving moves keys inside its zone only.** The other zones score the
  same nodes as before and hold their copies where they were, so losing a node
  does not disturb the copies that are meant to cover for it.
- **A zone that is gone is a copy that is gone.** The zones that are left hold
  what they held, and every key is still on one node in each of them.
- **A zone is not a replica set.** Nothing is designated primary, no zone is a
  follower of another, and there is no log shipped between them. Every copy is
  written by the node that took the request.

A cluster where no node names a zone is one zone containing all of them, which
is one copy of a key — the way this behaved before zones existed, and the way an
instance with no `ASYNCDB_ZONE` still behaves.

## One leader for each partition

Copies alone do not say what happens when two clients write the same key at the
same moment. Written to each copy independently, the copies can settle on
different values and stay that way. So one of the three copies of a partition
**leads** it, and every write of every key in that partition is ordered by that
one node.

A leader is claimed, not agreed. The node writes `/asyncdb/leader/{partition}`
into etcd **only if nothing has created it** — one transaction, so two claimants
are one leader and one node that is told the winner's name — and it claims on the
same lease its membership is on. A node that stops renewing therefore stops
leading, and the partition is claimed by whichever of its other copies gets there
first.

There is no election in the sense of votes: etcd already agrees with itself, and
this borrows that. It is the one thing the cluster has ever needed a coordinator
for.

A node claims **a few partitions at a time** — sixty-four on each pass of the
membership thread, walking the ring from an offset of its own name. Claiming
costs a round trip to etcd each, and there are 256 of them; taking a few at a
time keeps a node's start-up short, and starting from different offsets means
nodes claim different parts of the ring rather than racing each other for the
same partitions and losing. Between them, the nodes of a fresh cluster settle it
in a pass or two, and a partition nobody has claimed yet answers `no_leader` to a
write in the meantime.

### The term

The revision etcd created the claim at is the **term**, and it travels with every
write the leader orders, in `X-Asyncdb-Term`. Revisions only ever rise, so a
later leader of a partition always has a higher term than the leader before it,
and a copy that has applied a write of one term refuses anything older:

> `409 stale_leader` — the write was ordered by a node that has been replaced and
> does not know it yet.

That is what stops a leader which lost its lease, but not its network, from
writing behind the leader that replaced it.

## What a write and a read do

A write travels in two hops, and the term is what tells them apart — a write
*to* the leader carries none, because the node sending it is only asking for the
write to be ordered, and a write *from* the leader carries the term:

```
PUT /table/account/key/4821            the node asked, whichever it is
  └── PUT ...                          the node leading partition(4821)
        ├── writes its own copy        the leader holds one, always
        ├── PUT ... X-Asyncdb-Term: 41 the copy in the next zone
        └── PUT ... X-Asyncdb-Term: 41 the copy in the zone after that
```

The copies are written **at once rather than one after another**, so the leader
waits for the slowest of them rather than for the sum of them, and the thread it
is serving the write on is held for one round trip rather than for a copy each.

Every copy has to take it. A copy that refuses — because it is not there, because
its RocksDB is stalling, or because it has moved on to a later term — fails the
request, and the copies that took it keep what they took. Because they were all
asked at once, a copy that refused was asked beside the others and not ahead of
them: the refusal reported is the first in the order the zones are named, whichever
of them answered first. Writing a record is
idempotent, a key and a value or a key that is gone, so **the remedy is to run
the request again**, which is the remedy for a table create or a range delete
that one node refused as well.

A partition nothing leads yet has nowhere to order a write:

> `503 no_leader` — no node is leading this key's partition, so try again.

That is the window a leader's lease leaves when the node holding it goes away:
up to ten seconds, and then one of the other copies has claimed it. **Reads are
not in that window** — they are answered by a copy and never wait for a leader.

An instance standing alone, or a cluster with no zones, orders nothing: there is
one copy and nobody to race with, so a write is written where it always was.

A record is read from **one** copy: this node's own when it holds one, and
otherwise the copy in this node's own zone, which is the near one. A copy that
does not answer at all is passed over for the next, so a zone being down is a
zone being skipped rather than a read that fails. What a copy that *did* answer
says is the answer, a `404` included — every zone is written before a write is
answered, so one copy saying a key is not there is enough to say it is not
there.

With one exception. **A key this node holds nothing for is asked of the other
copies before it is answered as missing.** A node that has just replaced another,
or a zone that has just come back, owns its share of the keys and holds none of
what was written while it was away; without this it would answer `404` for
records the other zones still have, which is the one failure worth spending a hop
on a genuine miss to avoid. It is not read repair — the copy that was missing
stays missing until the record is written again.

## What each endpoint does in a cluster

| Endpoint | In a cluster |
| --- | --- |
| `PUT`/`DELETE` `/table/{table}/key/{key}` | Ordered by the node **leading the key's partition**, which writes the copy in every zone. Every copy has to take it |
| `GET`/`HEAD` `/table/{table}/key/{key}` | Answered by one copy: this node when it holds one, else the nearest that answers. A key this node holds nothing for is asked of the other copies |
| `PUT`/`DELETE` `/table/{table}` | Carried out on **every** node: a record can only be written where its table is |
| `GET` `/table`, `GET /table/{table}` | Answered where they are asked. Every node holds every table |
| `GET` `/table/{table}/key` | Asked of **one zone** — this node's own — and the pages merged back into key order |
| `DELETE` `/table/{table}/key` | Carried out on every node, because every node holds a share of the range |
| `GET /health` | Answered where it is asked, and names the nodes and zones it can see |

Nodes keep their connections to each other open between requests, so a forwarded
request is a request and not a handshake as well. A node that is shutting down
therefore says so: it finishes the requests it is serving, answers them with
`Connection: close`, and drops the connections that are only waiting, instead of
holding them until they time out.

A request a node passes on carries `X-Asyncdb-Forwarded: true`, and a node that
receives one serves it where it stands rather than passing it on again. Two nodes
that disagree about the membership for a moment can therefore give a stale
answer, but they cannot bounce a request between them.

`GET /health` names the cluster as this node sees it, which is the way to watch
a membership settle:

```json
{
  "status": "ok",
  "write_stalled": false,
  "nodes": [ "http://asyncdb-1:8080", "http://asyncdb-2:8080" ],
  "zones": {
    "eu-west-2a": [ "http://asyncdb-1:8080" ],
    "eu-west-2b": [ "http://asyncdb-2:8080" ]
  },
  "leads": 128
}
```

`leads` is how many of the 256 partitions this instance orders the writes of, and
it is the way to watch an election settle: a node that has just started leads
none of them, and writes of those partitions are refused until it or another copy
has claimed them.

`nodes` is absent, rather than a list of one, when the instance stands alone.
`zones` is absent when no node in the membership names one, so it is also the way
to see that a cluster meant to keep a copy per zone is keeping one: the number of
zones is the number of copies.

## Scans across a cluster

A scan is the one operation that cannot be answered by one node — but it can be
answered by one **zone**, and that is what it asks. A zone holds a copy of the
whole keyspace, so the nodes of one zone between them hold every key in the
range; the answers are merged into key order and cut to the `limit`.

The zone asked is **this node's own**, which is the cheap one: those nodes are in
the same availability zone as the node that was asked, so the pages cross no zone
boundary. A node that is alone in its zone holds every key itself and asks nobody
at all.

Asking every node instead would return the same records once per zone, and cost
three pages of bytes for every page of answer.

A node of that zone that does not answer is a **zone to give up on, not a scan to
fail**: another zone holds the same keys, so the whole range is asked of the next
one instead. Only when no zone has a complete set of nodes answering does the
scan fail. A node that *refuses* — a cursor this instance did not issue, a range
that is not below its end — is a different thing: every zone would refuse alike,
so the refusal is the answer and the next zone is not asked.

What is over the limit is dropped rather than held: the next page asks the zone
again from where this one ended, so the dropped keys are the keys the next page
begins with.

The cursor is issued by the node that answered, and names a position in the
merged order. It is still
[a cursor of one instance](/database/scans#paging-and-what-a-cursor-promises):
**page through a scan against the node that started it**, or use `from` and `to`,
which any node will take.

## What this is not

The partitioning is deliberately simple, and it is worth being plain about where
it ends.

- **There are as many copies as there are zones, and no more.** One zone is one
  copy, and the durability of a key is then the durability of one RocksDB. Three
  zones survive two of them being gone, and no arrangement here survives a key
  being written to a node whose disk is then lost before anything reads it: there
  is no log, no quorum and nothing to reconcile against.
- **Nothing repairs a copy that fell behind.** This is the one a leader does not
  fix. A write that one copy refused is answered as a failure, and the copies that
  took it keep it; there is no log to catch a copy up with, no read repair, no
  anti-entropy and no hinted handoff. Until the client runs the write again the
  zones disagree, and a read may be answered by either of them. A scan is answered
  by one zone, so it answers what *that* zone holds — a record another zone has
  and this one does not is a record the scan does not return.

  **A leader orders writes; it does not replicate them.** Ordering is what stops
  two clients diverging the copies. Catching a copy up is what a replication log
  would do, and there is not one.
- **A write needs a leader, and a leader needs etcd.** A partition whose leader
  has gone is unwritable until its lease runs out and another copy claims it —
  ten seconds at the outside — and a cluster that cannot reach etcd at all keeps
  the leaders it last read and elects no new ones. Reads never wait for any of
  this. That is the trade leadership makes: writes are ordered, and they are
  ordered by a node that has to be there.
- **A write is only as available as its least available zone.** Every copy has to
  take a write, so a zone that is down stops writes to the keys it holds while
  reads carry on from the zones that are up. Replication here is for reading
  through the loss of a zone, not for writing through it.
- **Records do not move when the membership changes.** A key that changes owner
  is a key the new owner does not have, and the old owner still does. A read
  still finds it, because the new owner asks the other zones for what it holds
  nothing of, and a scan still sees it, because the old owner is asked as well —
  but nothing rebuilds the copy in that zone until the record is written again,
  and a *write* lands on the new owner and leaves the old one holding a value
  that is now stale. Growing a cluster is therefore still a thing to do
  deliberately, at a quiet moment, and with the keys rewritten afterwards.
- **A node that joins has no tables.** Tables are created on every node that is a
  member at the time. Declare the tables a service needs at every start up, which
  is [what `PUT /table/{table}` is for](/database/tables#create-a-table), and a
  new node catches up on the next declaration.
- **An operation on every node that one node refuses fails the request**, after
  every other node has carried it out — they are all asked at once, so one
  refusing does not stop the rest. Creating a table, deleting a table and deleting
  a range are all idempotent, so the remedy is to run the request again.
- **A term is remembered in memory, not on disk.** A node that restarts has
  forgotten which terms it has applied, so it accepts the first write it is sent
  afterwards whatever term ordered it. A node that restarts has also lost its
  RocksDB, so this is a smaller hole than it sounds — but it is a hole, and
  persisting the term with the data is what closes it.
- **Nodes trust each other.** `X-Asyncdb-Forwarded` and `X-Asyncdb-Term` are
  honoured from anyone who sends them, so the API port belongs on a private network, exactly as it does
  without a cluster. The nginx in the image
  [clears the header](/deployment/network#the-api-port-is-not-the-load-balancers)
  from anything arriving on the `/asyncdb` prefix, so the port a browser reaches
  cannot claim it; the API port itself has no such guard.
