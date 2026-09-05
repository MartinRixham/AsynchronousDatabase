# Rebuilding a node

A replaced instance is an empty database: its root volume went with the instance,
and nothing in the cluster used to put that copy back — there is no read repair,
no anti-entropy, no hinted handoff and no replication log.

**A node now fills itself in on the way up.** Before it registers in etcd, an
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
4. Asks **every** node of that zone for its own share of each table, a page at a
   time, and writes back every record it will own.
5. Registers, and starts serving.

Two details of the API carry it:

| Used | Because |
| --- | --- |
| The forwarded header on every scan | A forwarded scan is served where it lands, so it answers one node's own share rather than its zone's merged copy — which is what lets a rebuild ask each node once and add up what they hold |
| `from=` paging, not cursors | A cursor names the instance that issued it, so a node that restarted under a long rebuild would refuse the next page. A key is a position any node will take |

A zone with a node that does not answer is a zone that cannot give the whole of
what it holds, so the **next zone is asked for the whole thing again** — the same
fallback a scan already makes. If no zone answers, the node starts with what it
has, which is what it would have had anyway.

The whole of it is bounded by a clock as well, at five minutes. Every round trip
inside has a timeout of its own, but how many of them there are is a count of
tables, pages and nodes — so the arithmetic that says "four timeouts is two
minutes" is only ever wrong in one direction. A rebuild that runs out of time
stops where it is and the node starts thin, which is a copy the cluster has
rather than one it is still waiting for. It will not try again: an empty store is
the only trigger, and the store is no longer empty.

Nothing the rebuild does is worth dying over either. A store that refuses a
write, or a neighbour that answers something unreadable, is logged and the node
starts — because a process that fell over here would fall over in the same place
when it was restarted, and never register at all.

Pages carry values, so they are asked in hundreds rather than in thousands: a
value may be sixteen megabytes, and a page is built whole in memory at both ends.

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
it appears in the membership it is already whole.

```bash
curl -s http://asyncdb-1:8080/health | jq '.nodes'
```

## What it does not do

- **It does not repair a node that is merely thin.** The trigger is an empty
  store. A node that lost one record to a write that one copy refused holds
  tables, so it rebuilds nothing — that record comes back when it is written
  again, and nothing else brings it back.
- **It is not continuous.** One pass, on the way up. A copy that falls behind
  afterwards stays behind.
- **It is not a backup.** It needs a zone that still holds the data. Lose every
  zone's copy of a partition and there is nothing to read from.
- **It does not move records when the membership changes.** Growing a cluster is
  still [a thing to do deliberately](/database/cluster#what-this-is-not).
- **It is not free.** It reads every key and value this node will own across a
  zone boundary, and the node is not serving while it does. On a large store that
  is a slower start-up, and the load balancer will hold traffic off until it is
  done.

## The one hazard

**A delete that lands during a rebuild can bring a record back.** There are no
tombstones: a record deleted after the source zone was read and before it is
written back is written back anyway, and the delete is undone.

The window is smaller than it looks, because the rebuilding node is not in the
membership — a delete during that window is not routed to it, and it is only the
*source* zone's view going stale that matters. But it is real, and it is the same
reason the docs say to grow a cluster at a quiet moment.
