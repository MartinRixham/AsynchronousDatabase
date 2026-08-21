# Tables

A table is a named keyspace, and a RocksDB column family. Names are `[a-z0-9_-]`,
between 1 and 64 characters. `default` is reserved for the column family RocksDB
always has.

## List the tables

```http
GET /table
```

```json
{
  "tables": [
    { "name": "account", "contentType": "application/json" },
    { "name": "transaction", "contentType": "application/json" }
  ]
}
```

Instances hold dozens of tables, so the list is not paged.

## Create a table

```http
PUT /table/account
Content-Type: application/json

{
  "contentType": "application/json",
  "compression": "lz4",
  "cacheIndexAndFilterBlocks": true
}
```

`201 Created` when the table is new, `200 OK` when it already existed with these
options, `409 Conflict` when it existed with different ones. Creating a table is
therefore safe to run at every start-up, which is how a service should declare
the tables it needs.

| Option | Default | Means |
| --- | --- | --- |
| `contentType` | `application/octet-stream` | The type every record in the table is stored and returned as |
| `compression` | `lz4` | `none`, `snappy`, `lz4` or `zstd`. Cold tables want `zstd`; tables of already-compressed values want `none` |
| `cacheIndexAndFilterBlocks` | `true` | Keeps index and bloom blocks in the shared block cache, so one large table cannot pin memory the others need |
| `prefixLength` | absent | Fixed-length key prefix. Set it when the table is scanned by a prefix of that length, and RocksDB can use bloom filters to skip files that hold no such prefix |

Options that are not offered are as much of the design as the ones that are.
There is no per-table comparator ([why](/database/#keys-are-bytes-and-ordering-is-byte-wise)),
no per-table write buffer — the instance shares one budget across column
families, and letting a client claim from it would let one table starve the
rest — and no merge operator, because a merge operator is code, and code is
deployed, not configured over HTTP.

## Inspect a table

```http
GET /table/account
```

```json
{
  "name": "account",
  "contentType": "application/json",
  "compression": "lz4",
  "estimatedKeys": 148203,
  "sizeBytes": 41863168,
  "levels": [ 4, 0, 21, 60, 0, 0, 0 ]
}
```

`estimatedKeys` and `sizeBytes` are RocksDB's own estimates
(`rocksdb.estimate-num-keys`, `rocksdb.total-sst-files-size`). They are cheap
and approximate: the key count counts entries not yet compacted, so a table
that has had many overwrites or deletes reads high. **An estimate is never the
answer to "how many records are there".** Counting exactly means scanning, and
scanning is [what scans are for](/database/scans).

`levels` is the number of SST files at each level, which is what tells an
operator whether compaction is keeping up.

## Delete a table

```http
DELETE /table/account
```

`204 No Content`, and the data is gone with the column family — no tombstones,
no wait for compaction. `404 Not Found` if there was no such table.

This is the only cheap way to delete a lot of data. Deleting the records of a
table one by one, or over a range, leaves tombstones behind
([delete a range](#delete-a-range)).

## Maintenance

Both of these are operator actions. They are in the API because they are
sometimes the answer to a performance problem, not because a client should call
them in normal use.

### Compact a table

```http
POST /table/account/compact
Content-Type: application/json

{ "from": "user:", "to": "user;" }
```

Runs a manual compaction over the range, or the whole table if `from` and `to`
are omitted. It reclaims the space held by deleted and overwritten records, and
it is expensive: it rewrites the files it touches. `202 Accepted`, and the work
happens in the background.

### Flush a table

```http
POST /table/account/flush
```

Writes the memtable out to an SST file. `204 No Content` once it is done. Useful
before taking a backup of the files; not a substitute for
[durability](/database/records#durability) on the writes themselves.

### Delete a range

```http
DELETE /table/account/key?prefix=user:2019
```

Deletes every record in the range in one operation, as a RocksDB range
tombstone rather than a delete per key. `204 No Content`.

Two things to know before using it:

- The space comes back at compaction, not at deletion. Until then the records
  are hidden, not gone, and the table's `sizeBytes` will not move.
- Range tombstones make reads that cross them slower, because every read in the
  range has to consult the tombstone. A table that accumulates many of them
  wants a compaction, or wants to have been a table that could be dropped
  whole.

It takes the same range parameters as a [scan](/database/scans#the-range), and
refuses a request that names no range at all — deleting every record in a table
is `DELETE /table/{table}` and then creating it again.
