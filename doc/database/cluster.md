# The cluster

One instance of asyncdb owns one RocksDB. Several instances own a keyspace
between them: each key lives on exactly one of them, and every instance answers
for every key, by asking the instance that holds it when it does not.

Instances find each other through **etcd**. There is no leader, no coordinator
and no configuration naming the other nodes: a node registers itself, reads who
else is registered, and computes the same answer as every other node to the only
question the cluster has to agree on — which node owns a key.

```
                      ┌────────┐
                      │  etcd  │   /asyncdb/node/<address> per node, on a lease
                      └───┬────┘
           ┌──────────────┼──────────────┐
           │              │              │
      ┌────┴────┐    ┌────┴────┐    ┌────┴────┐
      │ node 1  │────│ node 2  │────│ node 3  │   a key it does not own is
      └─────────┘    └─────────┘    └─────────┘   asked of the node that does
```

## Turning it on

Two environment variables, and nothing else:

| Variable | Is |
| --- | --- |
| `ASYNCDB_ETCD` | Where etcd answers. One base URL, or every member of the etcd cluster separated by commas |
| `ASYNCDB_NODE` | This node as the others reach it: `http://asyncdb-1:8080` |

```
ASYNCDB_ETCD=http://etcd-1:2379,http://etcd-2:2379,http://etcd-3:2379
```

Name all of them. Every member of an etcd cluster answers for the whole of it,
so a node that cannot reach the member it was using asks the next one and stays
with whichever answered. Naming one member makes that member a single point of
failure for the *membership* — records are still served, because a node that
cannot reach etcd carries on as a cluster of one, but nothing learns about
anything joining or leaving until it comes back.

Set neither and the instance is what it has always been: one process owning the
whole keyspace, talking to nothing. Set both and it joins.

`ASYNCDB_NODE` is the address of the **API port**, not of the nginx in front of
it — nodes talk to each other directly, and they do not go through the `/asyncdb`
prefix the browser uses.

etcd is spoken to over its JSON gateway rather than over gRPC, which is why
there is no gRPC dependency here: a node registers with `POST /v3/kv/put` and
libcurl, the same libcurl the API already uses.

## Membership

A node registers `/asyncdb/node/{address}` with a **lease** and renews it every
few seconds. The lease is what makes membership honest: a node that stops
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

## What each endpoint does in a cluster

| Endpoint | In a cluster |
| --- | --- |
| `GET`/`HEAD`/`PUT`/`DELETE` `/table/{table}/key/{key}` | Answered by the node that owns the key, whichever node was asked |
| `PUT`/`DELETE` `/table/{table}` | Carried out on **every** node: a record can only be written where its table is |
| `GET` `/table`, `GET /table/{table}` | Answered where they are asked. Every node holds every table |
| `GET` `/table/{table}/key` | Asked of every node, and the pages merged back into key order |
| `DELETE` `/table/{table}/key` | Carried out on every node, because every node holds a share of the range |
| `GET /health` | Answered where it is asked, and names the nodes it can see |

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
  "nodes": [ "http://asyncdb-1:8080", "http://asyncdb-2:8080" ]
}
```

The field is absent, rather than a list of one, when the instance stands alone.

## Scans across a cluster

A scan is the one operation that cannot be answered by one node. Every node is
asked for the same range, and the answers are merged into key order and cut to
the `limit`. What is over the limit is dropped rather than held: the next page
asks every node again from where this one ended, so the dropped keys are the keys
the next page begins with.

The cursor is issued by the node that answered, and names a position in the
merged order. It is still
[a cursor of one instance](/database/scans#paging-and-what-a-cursor-promises):
**page through a scan against the node that started it**, or use `from` and `to`,
which any node will take.

## What this is not

The partitioning is deliberately simple, and it is worth being plain about where
it ends.

- **There is one copy of every record.** Partitioning is not replication: a node
  that is down takes its share of the keys with it until it comes back, and a
  node that loses its disk loses them. The durability of a key is the durability
  of one RocksDB.
- **Records do not move when the membership changes.** A key that changes owner
  is a key the new owner does not have, and the old owner still does. Reads for
  it answer as though it were missing until the membership is what it was.
  Growing a cluster is therefore a thing to do deliberately, at a quiet moment,
  and with the keys rewritten afterwards.
- **A node that joins has no tables.** Tables are created on every node that is a
  member at the time. Declare the tables a service needs at every start up, which
  is [what `PUT /table/{table}` is for](/database/tables#create-a-table), and a
  new node catches up on the next declaration.
- **An operation on every node that one node refuses fails the request**, after
  the nodes that answered before it have carried it out. Creating a table,
  deleting a table and deleting a range are all idempotent, so the remedy is to
  run the request again.
- **Nodes trust each other.** `X-Asyncdb-Forwarded` is honoured from anyone who
  sends it, so the API port belongs on a private network, exactly as it does
  without a cluster.
