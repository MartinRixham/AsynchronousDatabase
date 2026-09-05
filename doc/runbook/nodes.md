# A node has failed

One instance is one process, one nginx and one RocksDB, and the ways it can be
gone are not all the same: a process that has stopped, a container that has
stopped, an instance that has been replaced, and a node that is up but cannot be
reached each look different from the outside and recover differently.

The cluster is built to survive all of them for **reads** — a copy that does not
answer is passed over — and to refuse **writes** while any copy of the key is
missing, because every copy has to take a write. So the shape of an outage here
is almost always *reads fine, writes refused*, and that is by design rather than
by accident.

## The container has stopped

**Looks like:** `502` or `504` with an HTML body from that instance, and nothing
at all on the API port. `/asyncdb/health` does not answer; `/` still serves the
UI, because nginx and the database are started together by the image's
`CMD nginx & ./asyncdb` and nothing makes one wait for the other.

**Check:**

```bash
docker ps -a --filter name=asyncdb
docker logs --tail 100 <container>
```

**Recovers by itself.** Both `docker-compose.yml` and the instance user data run
the container `--restart always`, so a process that crashed is restarted. What
the store holds survives it, because `/var/lib/asyncdb` is a volume — a named
one per node under compose, a bind of the host's root volume on AWS — so a
restarted container opens what the one before it wrote rather than an empty one.

**Does not recover by itself** when the container never started: an image that
cannot be pulled leaves no container for a restart policy to apply to. See
[instances are replaced in a loop](/runbook/deployment#instances-are-replaced-in-a-loop),
and note that on the current template
[nothing replaces that instance](/runbook/deployment#the-group-does-not-replace-a-failed-application).

**If it will not start**, the process refuses to come up for exactly one reason
of its own: RocksDB would not open the directory. That is
[the store is locked or will not open](/runbook/storage#the-store-will-not-open),
and it is written to the log before the process exits.

## A node does not answer

**Looks like:** reads succeed, writes fail with

> `500 storage_error` — `Node "http://asyncdb-2:8080" did not answer: ...`

and `/health` on the other nodes still names the missing one, for up to ten
seconds.

This is the window between a node going away and etcd dropping it. The node is
in the membership, so it is still one of the copies of its keys; it does not
answer, so every write to those keys fails. **It closes itself**: the lease is
ten seconds, the node renews every three, and when it stops renewing etcd removes
its key and the other nodes stop sending it anything.

| While it lasts | |
| --- | --- |
| Reads of keys it held | Answered — the copy that does not answer is passed over for the next |
| Writes of keys it held | Refused, `storage_error`. Run them again once it is out of the membership |
| Reads and writes of other keys | Unaffected |
| Scans | Unaffected while another zone is whole |
| Table create and delete | Refused, because they go to every node. Idempotent — run again |

**What to do:** wait ten seconds, ask `/health` again, and confirm the node has
left the membership. Then retry the writes that failed. If it is still named
after a minute, its process is alive enough to renew its lease but not to answer
requests, which is [a node that is up but wrong](#a-node-that-is-up-but-wrong).

There is one caveat worth knowing: a fan-out **does not stop** at the first
refusal. Every copy is asked at once, so a copy that refused was asked beside the
others rather than ahead of them, and the copies that took the write keep it. The
refusal reported is the first in the order the zones are named, whichever
answered first. That is why the remedy is to run the whole write again and not to
reason about which copies took it.

## A node that is up but wrong

Harder than a node that is down, because it stays in the membership: it renews
its lease on its own thread, and renewal says nothing about whether it can serve
a request.

**Symptoms:** requests to it hang and then fail; every node forwarding to it is
slow. A forwarded request waits `timeout_seconds`, which is **30 seconds**, and
the thread serving it waits with it.

**Check that node directly** rather than through the load balancer:

```bash
curl -s --max-time 5 http://asyncdb-2:8080/health | jq
curl -s --max-time 5 http://asyncdb-2:8080/table | jq
docker logs --tail 200 asyncdb-2
```

**Do:** stop it. A node stopped cleanly revokes its lease and is gone from the
membership at once rather than in ten seconds, and it finishes the requests it is
serving, answers them `Connection: close`, and cuts the connections that are only
waiting. `SIGTERM` and `SIGINT` both do that — `docker stop` sends `SIGTERM` — and
the process leaves on its own once serving has ended.

Taking a sick node out is nearly always better than leaving it in. Its keys are
held in every other zone, so reads carry on, and the writes it was failing start
succeeding as soon as it is out of the membership.

## Threads are all waiting

**Looks like:** a node stops answering *anything* under load, including its own
health check, while its CPU is idle.

A thread here spends most of a forwarded request waiting on another node, so the
pool is a count of requests that can be in flight rather than a count of cores.
It is `ASYNCDB_THREADS` if that is set to a positive number, and otherwise
**eight threads per core, clamped between 16 and 128**. Two threads on a
two-core instance would be a server that two waiting requests fill — health
check included, which is what has an instance taken out of service.

**Do:** raise `ASYNCDB_THREADS` on that node, and look for what the threads are
waiting on, which is usually a neighbour that is up but wrong. Each thread keeps
its own curl handles, so the pool is also how many connections this node holds to
each neighbour — a very large number is more connections than a neighbour wants.

## A scan fails while everything else works

A scan is asked of **one zone**, because a zone holds a copy of the whole
keyspace, and every node of that zone has to answer for the scan to be complete.
A node of it that does not answer is a **zone to give up on, not a scan to
fail**: the whole range is asked of the next zone instead.

So a scan that fails means **every zone has a node that does not answer**, while
reads and writes of individual keys are still fine — they only need one copy and
one leader.

**Check** each node in each zone directly:

```bash
curl -s http://localhost:8080/asyncdb/health | jq '.zones'
```

then ask each named node for `/health` on its API port. The one that does not
answer is the one to deal with, by the sections above.

A scan that fails with a `4xx` is a different thing entirely and is not about
nodes: every zone would refuse it alike, so the refusal is the answer and no
other zone is asked. That is
[`invalid_cursor`](/runbook/errors#_400-invalid-cursor-part-way-through-a-scan) or
`invalid_range`.

## An instance was replaced

**This is the one that loses data.** A replaced instance is a new root volume,
which is an empty database — the volume is `DeleteOnTermination`, there is no
snapshot and no backup.

The node comes back, registers, and owns its share of the keys in its zone while
holding none of what was written before. What that looks like:

- **Reads still work.** A key this node holds nothing for is asked of the other
  copies before it is answered as missing, so the other zones answer for it.
- **Scans of its zone are short.** A scan is answered by one zone, so it answers
  what *that* zone holds. The records this node lost are not in its zone's answer.
- **`table_not_found` for the keys it owns**, until the tables are declared
  again — because the tables went with the volume too.
- **Writes land on it** and are ordered normally, so new data is fine.

**It usually recovers itself.** A node that comes up with an empty store
[rebuilds before it registers](/runbook/rebuild): it reads a zone that still
holds its records and writes back the tables and the records it owns. It is not
in the membership while it does that, so nothing waits on it, and by the time it
appears in `/health` it is whole.

**When it does not** — an instance that is not clustered, a cluster with only one
zone, or a start-up where no other zone answered — the recovery is by hand:

1. Declare every table again. `PUT /table/{table}` is idempotent and goes to
   every node, so one call per table repairs the schema across the cluster.
2. Write the records again. There is nothing else that puts them back — no read
   repair, no anti-entropy, no hinted handoff and no log to catch the copy up
   with.

The scale of this is worth being plain about: on the six-instance stack each
node holds half of one zone's copy, so one replacement is **one sixth of the
cluster's copies** gone, and the keys are still readable from two other zones.
Losing all three copies of a partition is losing those records.

See [the store](/runbook/storage#a-node-came-back-empty) for how to tell an
empty node from a node that is merely missing a table.

## Shutting a node down deliberately

The clean path, and it is worth using it:

```bash
docker stop asyncdb-2      # SIGTERM
```

In order, the process leaves the cluster **before** it stops accepting: it
revokes its lease so the other nodes stop sending it keys, then closes the
acceptor, then cuts the sessions that are only waiting and lets the busy ones
finish with `Connection: close`. A node that stopped this way is out of the
membership at once rather than at the end of its lease, so the window of writes
being refused is nearly nothing.

Revoking is best effort. If etcd is already gone — everything is often shut down
together — the node says so and leaves, and etcd drops it when the lease runs
out. It spends one timeout on this rather than one per member, because a node
being stopped has ten seconds before it is stopped for good.

**Take one node at a time**, and let each come back before the next goes. Each
one down is a zone's copy of its keys unwritable, and two zones down for the same
partition is more than that.
