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
| `GET` | `/table/{table}/key` | [Scan a range](/database/scans) |
| `DELETE` | `/table/{table}/key` | [Delete a range](/database/tables#delete-a-range) |
| `GET` | `/health` | Liveness, whether writes are stalled, and [the nodes of the cluster](/database/cluster#what-each-endpoint-does-in-a-cluster) |

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
| `invalid_table_name` | 400 | Not 1–64 characters of `[a-z0-9_-]`, or `default` |
| `dependency_not_found` | 400 | A name in [`dependencies`](/database/tables#dependencies) is not a table |
| `invalid_key_encoding` | 400 | A key in the path does not percent-decode to valid UTF-8 |
| `key_too_large` | 413 | Over 4 KiB |
| `value_too_large` | 413 | Over 16 MiB |
| `invalid_range` | 400 | A range whose `from` is not below its `to`, or a range delete with no bounds |
| `invalid_cursor` | 400 | A cursor this instance did not issue |
| `precondition_failed` | 412 | The conditional write did not hold |
| `write_stalled` | 503 | RocksDB is applying back pressure |
| `storage_error` | 500 | RocksDB returned an error |

## Limits

Keys and values are strings, and
[every string is valid](/database/records#keys-and-values). The limits below are
all that constrain them, and the sizes are counted in UTF-8 bytes.

| Limit | Value | Why |
| --- | --- | --- |
| Key | 4 KiB | Keys live in indexes and bloom filters, which are held in memory |
| Value | 16 MiB | A value is read whole into memory to be served |
| Scan `limit` | 1000, default 100 | One page is one response, held in memory |
| Table name | 64 characters | |
| Tables | dozens | [Each is a memtable](/database/#tables-are-column-families) |

## The cluster

| Variable | Is |
| --- | --- |
| `ASYNCDB_ETCD` | Where etcd answers. One base URL, or every member of the etcd cluster separated by commas. Unset is one instance on its own |
| `ASYNCDB_NODE` | This node as the other nodes reach it. Unset is one instance on its own |

| Header | Means |
| --- | --- |
| `X-Asyncdb-Forwarded` | Another node sent this request here. It is served where it lands |

See [the cluster](/database/cluster) for what each endpoint does when there is
more than one instance, and for what partitioning does not do.
