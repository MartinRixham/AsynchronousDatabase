# The store

One instance is one RocksDB, in one directory, named by `ASYNCDB_DATA` and
defaulting to `/var/lib/asyncdb`. A table is a **column family**, and the table
document lives in the default one under `TABLE_<name>`.

Two consequences run through everything on this page:

- **Dropping a table drops its column family**, so the data goes with it, on
  every node, and there is nothing to undo it with.
- **The directory is opened as it stands**, not something random underneath it,
  so an instance started again over the same volume opens what the one before it
  wrote.

## Writes are stalled

**Looks like:** `503 write_stalled`, and `"write_stalled": true` in `/health` on
the node doing it.

> `Writing record "..." was refused: Incomplete: ...`

RocksDB is stopping or delaying writers because memtables or level zero have
backed up — compaction is not keeping up with the write rate. It is **back
pressure and not a failure**, which is why it is told apart from an error at all:
`Incomplete`, `Busy` and `TryAgain` become `write_stalled`, and everything else
becomes `storage_error`.

`/health` reports it from two RocksDB properties, so it is true while writers are
either stopped outright or being delayed:

| Property | |
| --- | --- |
| `kIsWriteStopped` | Writers are stopped |
| `kActualDelayedWriteRate` | Writers are being slowed |

**Do:**

1. **Back off.** Writing harder makes it worse; this is the mechanism working.
2. Find which node. `write_stalled` is per-node — a stall on one node is a stall
   on one node's share of the keyspace, and because a write goes to every zone,
   one stalled node refuses writes to every key it holds a copy of.
3. Give it time. It clears in seconds to minutes as compaction catches up.
4. If it does not clear, look at the disk. A stall that never ends is usually
   IO that is too slow, or a disk that is nearly full — the AWS instances have a
   30 GB `gp3` root volume shared with the image, the logs and everything else.

```bash
df -h /var/lib/asyncdb
docker exec asyncdb-1 du -sh /var/lib/asyncdb
```

**Do not** delete files in the directory to make room. That is
[a store that will not open](#the-store-will-not-open) and an empty node after
it.

## RocksDB returned an error

**Looks like:** `500 storage_error` with the RocksDB status in the message.

> `Writing record "..." failed: IO error: No space left on device`
>
> `Creating table "..." failed: Corruption: ...`

The status is the diagnosis, and the common ones are:

| Status | Is | Do |
| --- | --- | --- |
| `IO error: No space left on device` | The volume is full | Free space or replace the instance. Writes fail until then |
| `IO error` otherwise | The disk or filesystem | Check the host; the instance is usually the fix |
| `Corruption` | Damaged SST files | **The store is not recoverable here.** Replace the node and rewrite the data |
| `NotFound` on a column family | A table that was dropped mid-request | Declare the table again |

There is no repair tool wired in and no backup to restore from, so **the
recovery for a genuinely broken store is to replace it and write the data
again**, which is the same recovery as
[a node that came back empty](#a-node-came-back-empty). The other zones hold
their copies throughout, so reads carry on while this is done.

## The store will not open

**Looks like:** the process exits at start-up and the container restarts in a
loop. The log says so before it goes:

> `Failed to open rocksdb with status: IO error: While lock file ... Resource temporarily unavailable`

**RocksDB locks the directory it opens**, so this is two processes over one
directory. Causes, in order of likelihood:

- **Two containers on one volume.** A container that was not fully stopped still
  holds the lock while a new one starts. `docker ps -a`, and stop the old one.
- **Two servers in one process**, which is only a testing concern: `cluster_test`
  gives its two servers `/tmp/asyncdb/first` and `/tmp/asyncdb/second` for
  exactly this reason.
- **`ASYNCDB_DATA` pointing at a directory another node owns**, which on compose
  is two services sharing a volume.

Anything else in that message — `Corruption`, an `IO error` that is not a lock —
is a store that will not open again. Replace it.

```bash
docker logs asyncdb-1 2>&1 | grep -i rocksdb
docker inspect asyncdb-1 --format '{{range .Mounts}}{{println .Source " -> " .Destination}}{{end}}'
```

## A node came back empty

**Looks like:** `404 table_not_found` for tables that plainly exist elsewhere,
and records missing from that node's zone, on a node that is otherwise healthy
and in the membership.

Tell it from a node that is merely missing one table by asking it what it has:

```bash
curl -s http://asyncdb-2:8080/table | jq
```

Nothing at all is an empty store. A subset is a node that joined after some
tables were declared.

**Why it happens:**

| Cause | Data | |
| --- | --- | --- |
| Container restarted | **Kept** | The volume outlives the container |
| Host rebooted | **Kept** | Same volume |
| Instance replaced (AWS) | **Gone** | The root volume is `DeleteOnTermination` |
| Compose volume removed (`down -v`) | **Gone** | |
| Node joined the cluster later | Never had it | Tables go to the members at the time |

**Recovery:**

1. **Declare every table again.** It is idempotent and goes to every node, so
   one pass over the schema fixes every node at once:

   ```bash
   for t in account transaction summary; do
     curl -sX PUT "http://localhost:8080/asyncdb/table/$t" \
       -H 'Content-Type: application/json' -d '{"dependencies":[]}' -o /dev/null -w "$t %{http_code}\n"
   done
   ```

   Declare dependencies **after** the tables they name, because a dependency on a
   table that does not exist is `dependency_not_found` — which is what keeps the
   graph free of dangling edges.

2. **Write the records again.** Nothing else puts them back. The other zones hold
   their copies, and no mechanism here copies one zone's records into another:
   no read repair, no anti-entropy, no hinted handoff, no replication log.

Until step 2 is done, the cluster is not broken but it is thinner than it looks:
reads are answered by the zones that still hold the record, and a **scan of the
empty node's zone will not return it**, because a scan is answered by one zone
and answers what that zone holds.

The best defence is the one the API is built for: **have every service declare
the tables it needs at start-up.** A node that joins then catches up on the next
declaration rather than on an operator noticing.

## The disk is filling

Nothing here caps or watches the size of the store. Values are up to 16 MiB, a
range delete is a `DeleteRange` and does not free space until compaction runs,
and the root volume is 30 GB shared with everything else on the instance.

```bash
df -h /
docker system df
```

**Do:** delete what is not needed — a table, or a range — and let compaction
catch up. Dropping a table frees the most, because the column family goes with
it. Watch for
[the stall](#writes-are-stalled) clearing afterwards rather than assuming it
did.

## What there is not

Worth saying plainly, because most of the recovery steps above are shaped by it:

- **No backup and no snapshot.** The AWS root volume is `DeleteOnTermination`;
  nothing is exported anywhere.
- **No repair.** A corrupt store is replaced, not fixed.
- **No rebuild of a copy.** A zone that lost its copy of a key does not get it
  back from the zones that still have it.
- **No rebalancing.** A key that changes owner is a key the new owner does not
  have and the old owner still does. A read still finds it, because a node asks
  the other copies for what it holds nothing of, but a **write** lands on the
  new owner and leaves the old one holding a value that is now stale. That is
  why growing a cluster is a thing to do deliberately, at a quiet moment, with
  the keys rewritten afterwards.
