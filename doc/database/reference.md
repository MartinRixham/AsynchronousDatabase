# Reference

Every endpoint, status code, error code and limit in one place.

## Endpoints

| Method | Path | Does |
| --- | --- | --- |
| `GET` | `/table` | [List tables](/database/tables#list-the-tables) |
| `PUT` | `/table/{table}` | [Create a table](/database/tables#create-a-table) |
| `GET` | `/table/{table}` | [Inspect a table](/database/tables#inspect-a-table) |
| `DELETE` | `/table/{table}` | [Delete a table](/database/tables#delete-a-table) and its data |
| `GET` | `/table/{table}/key/{key}` | [Read a record](/database/records#read-a-record) |
| `HEAD` | `/table/{table}/key/{key}` | Existence and size of a record |
| `PUT` | `/table/{table}/key/{key}` | [Write a record](/database/records#write-a-record) |
| `DELETE` | `/table/{table}/key/{key}` | [Delete a record](/database/records#delete-a-record) |
| | `/table/{table}/key/{key}/{sort}` | The same four, of a record with a [sort key](/database/records#partition-keys-and-sort-keys) |
| `GET` | `/table/{table}/key` | [Scan a range](/database/scans) |
| `DELETE` | `/table/{table}/key` | [Delete a range](/database/tables#delete-a-range) |
| `GET` | `/table/{table}/file` | One node asking another for [its share of a table](/database/cluster#moving-a-share-of-a-table), as records or as `values=false` keys. Between nodes, not for clients |
| `GET` | `/table/{table}/split` | Where the node being read would [cut a walk of its table up](/database/cluster#several-pieces-at-once), so that several pieces of it can be read at once. Between nodes, not for clients |
| `GET` | `/health` | Liveness, whether writes are stalled, whether the node [holds less than it owns](/runbook/rebuild), [the nodes and zones of the cluster](/database/cluster#what-each-endpoint-does-in-a-cluster) and how many partitions this node leads |

## Errors

Every error carries the same body, and the `code` — not the message and not the
status — is what a client should branch on.

```json
{
  "error": {
    "code": "table_not_found",
    "message": "No table named account."
  }
}
```

| Code | Status | Means |
| --- | --- | --- |
| `table_not_found` | 404 | No table of that name |
| `table_exists` | 409 | The table exists with different options |
| `record_exists` | 409 | The key has been written and its table is [immutable](/database/tables#immutable-tables) |
| `table_immutable` | 409 | A record or a range of an [immutable](/database/tables#immutable-tables) table cannot be deleted |
| `invalid_table_name` | 400 | Not 1–64 characters of `[A-Za-z0-9_ -]`, or `default` |
| `invalid_body` | 400 | The body of a [table](/database/tables#create-a-table) is not a JSON object, or an option in it is not of the type that option takes |
| `dependency_not_found` | 400 | A name in [`dependencies`](/database/tables#dependencies) is not a table |
| `invalid_key_encoding` | 400 | A key in the path does not percent-decode to valid UTF-8 |
| `key_too_large` | 413 | Over 4 KiB |
| `value_too_large` | 413 | Over 16 MiB |
| `invalid_range` | 400 | A range whose `from` is not below its `to`, or a range delete with no bounds |
| `invalid_cursor` | 400 | A cursor this instance did not issue, or a file resumed at something that is not base64 |
| `invalid_partitions` | 400 | A [file](/database/cluster#moving-a-share-of-a-table) asked for with something that is not a set of this cluster's partitions |
| `write_stalled` | 503 | RocksDB is applying back pressure |
| `no_leader` | 503 | No node is [leading this key's partition](/database/cluster#one-leader-for-each-partition) yet. Run the write again |
| `node_incomplete` | 503 | The node asked holds less than it owns, so it cannot say the key is missing. A read is asked of the next copy instead; `/health` names the node it came from |
| `stale_leader` | 409 | The write was ordered by a node that has since been replaced. Run it again |
| `storage_error` | 500 | RocksDB returned an error |
| `unavailable` | 500, 502, 504 | The nginx in front of the database answered instead of it. This one is the proxy's, not the server's — it is what a client sees while an instance is starting, once its container has stopped, or when a body too large to hold in memory could not be [spooled onto a full disk](/runbook/storage#the-disk-is-filling) |

## Limits

Keys and values are strings, and
[every string is valid](/database/records#keys-and-values). The limits below are
all that constrain them, and the sizes are counted in UTF-8 bytes.

| Limit | Value | Why |
| --- | --- | --- |
| Key | 4 KiB | Keys live in indexes and bloom filters, which are held in memory. It is the whole key: a partition key, its sort key and the byte between them |
| Value | 16 MiB | A value is read whole into memory to be served |
| Scan `limit` | 1000, default 100 | One page is one response, held in memory |
| Scan page | 8 MiB | The same reason counted in bytes, because a limit cannot see the size of what it lets through: a page ends early and carries a cursor rather than building a response the node cannot hold |
| File | 64 MiB walked | What one [transfer between nodes](/database/cluster#moving-a-share-of-a-table) reads of a table. Far larger than a page, because past this a transfer waits on the bandwidth between two nodes rather than on the time to ask for it. A walk of keys alone reads no values, so it covers far more of a table for the same budget. It is the whole walk's budget: reading a share [in several pieces at once](/database/cluster#several-pieces-at-once) shares it out between them |
| Table name | 64 characters | |
| Tables | dozens | [Each is a memtable](/database/#tables-are-column-families) |

## The cluster

| Variable | Is |
| --- | --- |
| `ASYNCDB_ETCD` | Where etcd answers. One base URL, or every member of the etcd cluster separated by commas. Unset is one instance on its own |
| `ASYNCDB_NODE` | This node as the other nodes reach it. Unset is one instance on its own |
| `ASYNCDB_ZONE` | The availability zone this node is in. Every zone holds [one copy of every record](/database/cluster#one-copy-in-every-zone). Unset is one zone, which is one copy |

| Variable | Is |
| --- | --- |
| `ASYNCDB_DATA` | The directory the store is kept in. Default `/var/lib/asyncdb` |
| `ASYNCDB_THREADS` | How many threads serve requests. Default eight a core, between 16 and 128 |
| `ASYNCDB_MEMORY` | Mebibytes the store may hold in memory — the block cache and the memtables together. Default 512. It is the one number to size to the instance: a node holding a share of a terabyte wants a great deal more of it than a node in a test |

| Header | Means |
| --- | --- |
| `X-Asyncdb-Forwarded` | Another node sent this request here. It is served where it lands |
| `X-Asyncdb-Term` | The [term](/database/cluster#the-term) the leader of the key's partition ordered this write in. A copy refuses anything older |
| `X-Asyncdb-Records` | On the answer to a `file`: how many records it carries |
| `X-Asyncdb-Next` | On the answer to a `file`: base64 of the key the walk reached, and absent when it reached the end of the table |

See [the cluster](/database/cluster) for what each endpoint does when there is
more than one instance, and for what partitioning and replication do not do.
