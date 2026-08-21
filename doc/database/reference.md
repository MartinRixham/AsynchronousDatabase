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
| `POST` | `/table/{table}/compact` | [Compact](/database/tables#compact-a-table) |
| `POST` | `/table/{table}/flush` | [Flush](/database/tables#flush-a-table) |
| `GET` | `/health` | Liveness, and whether writes are stalled |

## Headers

| Header | On | Means |
| --- | --- | --- |
| `X-Key-Encoding` | Any request | [`text` or `base64url`](/database/records#key-encoding), for every key in the request and response |
| `Durability` | Writes | [`none`, `wal` or `sync`](/database/records#durability) |
| `If-Match`, `If-None-Match` | `PUT`, `DELETE` | [Conditional writes](/database/records#conditional-writes) |
| `ETag` | Read responses | Hash of the value bytes |
| `Retry-After` | `503` | Seconds to wait before retrying |

## Status codes

| Code | When |
| --- | --- |
| `200 OK` | A read that found something; a create that changed nothing |
| `201 Created` | A table created; a record created under `If-None-Match: *` |
| `202 Accepted` | A compaction started |
| `204 No Content` | A write, a delete, a flush |
| `304 Not Modified` | A read whose `If-None-Match` matched |
| `400 Bad Request` | The request is malformed. See the error code |
| `404 Not Found` | No such table or key |
| `409 Conflict` | A table exists with different options |
| `412 Precondition Failed` | An `If-Match` or `If-None-Match` on a write did not hold |
| `413 Payload Too Large` | Over a [limit](#limits) |
| `415 Unsupported Media Type` | The body's type is not the table's declared type |
| `500 Internal Server Error` | RocksDB failed. The instance may be unhealthy |
| `503 Service Unavailable` | Writes are stalled |

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
| `invalid_key_encoding` | 400 | A key is not valid UTF-8, or not valid base64url |
| `key_too_large` | 413 | Over 4 KiB |
| `value_too_large` | 413 | Over 16 MiB |
| `invalid_range` | 400 | A range whose `from` is not below its `to`, or a range delete with no bounds |
| `invalid_cursor` | 400 | A cursor this instance did not issue |
| `precondition_failed` | 412 | The conditional write did not hold |
| `write_stalled` | 503 | RocksDB is applying back pressure |
| `storage_error` | 500 | RocksDB returned an error |

### Back pressure is a status code

RocksDB slows writers down when memtables or level zero back up, and if the API
hid that, a client would see it as latency and answer it by sending more.
`503 write_stalled` with a `Retry-After` says what is happening, so a client can
back off deliberately. `GET /health` reports the same condition, so an operator
can see it before the clients do.

## Limits

| Limit | Value | Why |
| --- | --- | --- |
| Key | 4 KiB | Keys live in indexes and bloom filters, which are held in memory |
| Value | 16 MiB | A value is read whole into memory to be served |
| Scan `limit` | 1000, default 100 | One page is one response, held in memory |
| Table name | 64 characters | |
| Tables | dozens | [Each is a memtable](/database/#tables-are-column-families) |
