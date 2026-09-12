# What a client sees

Every error the API gives carries the same body, and the `code` is what to
branch on — not the message, which names nodes and RocksDB statuses and changes,
and not the status, which several codes share:

```json
{
  "error": {
    "code": "no_leader",
    "message": "No node is leading this key's partition yet."
  }
}
```

[The reference](/database/reference#errors) lists them all. This page is the
same list read the other way round: what to *do* about each one.

## Is it worth retrying?

| Code | Status | Retry | Because |
| --- | --- | --- | --- |
| `no_leader` | 503 | **Yes, after a second** | An election is in flight and will settle |
| `write_stalled` | 503 | **Yes, backing off** | Back pressure, not failure |
| `node_incomplete` | 503 | **Yes, once** | The node asked holds less than it owns; another copy can answer |
| `stale_leader` | 409 | **Yes, at once** | The write went to a leader that had been replaced |
| `storage_error` | 500 | **Yes, once** | A node did not answer; another may |
| `table_not_found` | 404 | Only after declaring the table | The table is not there. A node that came back empty answers `node_incomplete` instead, so this one is not a node's own gap |
| `invalid_cursor` | 400 | No — restart the scan | The cursor belongs to another instance, or to another partition |
| `invalid_partition` | 400 | No — name a partition | A scan named none of the 256, both ways at once, or one that is not one of them |
| `table_exists` | 409 | No | The table is there with different options |
| everything else 4xx | 400, 413 | No | The request is wrong and will stay wrong |
| `unavailable` | 500, 502, 504 | **Yes** | nginx's, not the server's — the database is not up yet, or [its disk is full](/runbook/storage#the-disk-is-filling) |

Every write in this API is idempotent — a key and a value, a table with its
options, a table that is gone — so **running the whole request again is always
safe**, and it is the documented remedy for every failure a write can be
given.

## `503 no_leader`

Two different things say this, and the message tells them apart.

> `No node is leading this key's partition yet.`
>
> `No node is leading the tables yet.`

Nothing has claimed the key's partition, or the partition that
[leads the tables](/database/cluster#the-tables-are-led-too), which is what a
table create or delete is ordered by. Either the cluster is cold and has not
finished claiming all 256 partitions, or the leader's node went away and its
lease has not run out yet. **Wait a second or two and write again.** Reads are
unaffected throughout — they never wait for a leader.

> `This node does not lead this key's partition.`
>
> `This node does not lead the tables.`

A write arrived forwarded at a node that does not lead the partition, which is
two nodes disagreeing about who leads it. It is refused rather than passed on
again, so that a disagreement cannot bounce a write between nodes for ever.
This settles as the membership does. If it persists, the nodes disagree about
the membership itself — see
[the membership is wrong](/runbook/membership#the-membership-is-wrong).

Neither is data loss, and neither needs a hand. What makes them last longer than
a few seconds is etcd being unreachable, because nothing new is elected then:
[etcd cannot be reached](/runbook/membership#etcd-cannot-be-reached).

## `409 stale_leader`

> `This key is led in a later term than the one that ordered this write.`

A copy was sent a write ordered in an older term than one it has already
applied — a leader that lost its lease, but not its network, writing behind the
leader that replaced it. The fencing worked: the older leader's write was
refused, which is the point of it.

**Run the write again.** It goes to the current leader and is ordered in the
current term. Seeing this steadily rather than once around a node's departure
means leadership is moving constantly, which is a node that keeps failing to
renew its lease — [membership](/runbook/membership#leadership-keeps-moving).

## `503 write_stalled`

RocksDB is stopping or delaying writers on the node that took the write, because
memtables or level zero have backed up. It is back pressure and not failure,
which is why it is told apart from an error at all.

**Back off and retry.** Writing harder makes it worse. `write_stalled` in
`/health` says which node is doing it, and
[the store](/runbook/storage#writes-are-stalled) says what to look at.

## `500 storage_error`

One code, two very different causes, and the message is what separates them.

> `Node "http://asyncdb-2:8080" did not answer: ...`
>
> `Node "http://asyncdb-2:8080" answered with something that is not a document.`
>
> `No node holding this key answered.`

A node in the cluster is unreachable or answering nonsense. Nothing is wrong
with the store — go to [a node does not answer](/runbook/nodes#a-node-does-not-answer).

> `Writing record "..." failed: Corruption: ...`
>
> `Creating table "..." failed: IO error: ...`

RocksDB itself refused, and the status it returned is in the message. Go to
[RocksDB returned an error](/runbook/storage#rocksdb-returned-an-error).

There is also a catch-all, which is any other exception escaping the router:

> `Failed to respond due to error: ...`

That is a bug rather than an operational condition. Capture the message and the
request that produced it.

## `404 table_not_found`

The node that answered has no such table. In a cluster that is one of two
things:

- **The table was never created on this node**, because the node joined after
  the table was declared. Tables are created on every node that is a member at
  the time, and nothing back-fills one that joins later.
- **The node came back empty**, which is an instance that was replaced.

Both have the same remedy, and it is the one the API is designed around:
**declare the tables again.** `PUT /table/{table}` is idempotent — the same
options are a `200` — so a service that declares the tables it needs at every
start-up repairs this by starting.

```bash
curl -sX PUT http://localhost:8080/asyncdb/table/account \
  -H 'Content-Type: application/json' -d '{"dependencies":[]}'
```

The records are a different matter: see
[a node came back empty](/runbook/storage#a-node-came-back-empty).

## A record that should be there is not

A read is answered by **one** copy, and what a copy that answered says is the
answer — a `404` included. That is sound when every zone was written, which is
what a successful write means. It is not sound when a write **failed** and the
client did not run it again: the copies that took it kept it, the copy that
refused did not, and a read may be answered by either.

So a record that comes and goes between reads is a write that was reported as
failed and never retried. **Write it again**, which is the only thing that puts
the copy back — there is no read repair here.

One case is handled: a key this node holds *nothing* for is asked of the other
copies before it is answered as missing, so a node that was replaced does not
answer `404` for records the other zones still have. It costs a hop on a genuine
miss, and it is worth it.

If a whole table's worth of records is missing, check the table was not dropped:
dropping a table drops its column family, and the data goes with it, on every
node.

## `400 invalid_cursor` part way through a scan

> A cursor names the partition it was issued for and the instance that issued it.

A load balancer is not the cause of this one: a scan names a partition, and a
partition routes every page of it to the same node — the copy that holds it — so
a cursor comes back to the node that issued it whichever node the client asks.

What is left is the two cases that are real:

- **The partition moved.** A membership change gives it to another node, and that
  node did not issue this cursor. Start the scan again; the page it answers is
  the same range.
- **The cursor was given back against a different partition.** It is a position
  in the partition it was issued for and nowhere else, so this is refused rather
  than answered with the nothing that key holds there.

A cursor also stops being valid when the instance that issued it restarts,
because the name is generated afresh each time the repository is opened. `from`
and `to` are the alternative, and any copy will take them.

## `502` or `504` with an HTML body

That answer is nginx's, not the server's, so it carries no `code` at all. The
database did not answer the proxy in front of it: the process is starting, or it
has stopped. Go to [the container has stopped](/runbook/nodes#the-container-has-stopped).

## The 4xx that are just wrong requests

These need a client change, not an operator:

| Code | The request |
| --- | --- |
| `invalid_table_name` | Not 1–64 characters of `[A-Za-z0-9_ -]`, or is `default` |
| `invalid_body` | The table body is not a JSON object |
| `dependency_not_found` | Names a table that does not exist — which is what keeps the graph free of dangling edges |
| `invalid_key_encoding` | The key does not percent-decode to valid UTF-8 |
| `key_too_large` / `value_too_large` | Over 4 KiB / 16 MiB |
| `invalid_range` | `from` is not below `to` |
| `table_exists` | The table exists with *different* options. The same options again are a `200` |
| `method_not_allowed` | A method the route does not have — something other than GET, HEAD, PUT or DELETE at all, or a DELETE of a record or a range, which only a table has |
| `invalid_path` | A path segment that is `..`, or a target that does not begin with `/` |

`dependency_not_found` on a table that plainly exists is the cluster case again:
a node that joined late does not have it. Declare the dependency on that node
first.
