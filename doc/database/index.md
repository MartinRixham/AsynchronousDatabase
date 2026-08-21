# The database API

An HTTP interface to a single RocksDB instance.

RocksDB is an embedded, ordered key-value store: it is a library inside one
process, with no network interface of its own. This API is the service that
gives it one — a small process that owns the RocksDB instance exclusively and
answers HTTP on its behalf. Everything the API offers is something RocksDB can
do; nothing it offers hides a cost that RocksDB does not have.

## What the API is for

- Reading and writing single records by key.
- Reading **ranges** of records in key order. This is the reason to put RocksDB
  behind an API rather than a hash-based store: keys are ordered, so a scan from
  a prefix is cheap and a scan of a range is a seek plus a walk.

It is not a query language. There are no secondary indexes, no joins and no
server-side filtering beyond the key range: what you can ask for is what the
key order can answer. Anything else belongs in the shape of the keys.

## The resource model

| Resource | Path | Is |
| --- | --- | --- |
| Instance | `/` | The one RocksDB instance this service owns |
| Table | `/table/{table}` | A named, independent keyspace |
| Record | `/table/{table}/key/{key}` | One key and its value |
| Range | `/table/{table}/key` | The records between two keys |

**Every path segment naming a resource is singular.** `/table/account/key/4821`
is one key in one table, and the path reads as what it addresses rather than as
the set it was drawn from. The segment does not change when the path addresses
several of them: `GET /table` lists the tables and `GET /table/account/key`
scans a range of keys, so a client never has to remember which endpoints
pluralise and which do not.

The rule is about paths. A JSON field that holds a list is still named for the
list — `records`, `tables` — because it names a part of a document, not a
resource, and calling an array `record` would say the wrong thing about what is
inside it.

Read the pages in that order: [tables](/database/tables),
[records](/database/records), [scans](/database/scans), and the
[reference](/database/reference) for status codes, error codes and limits.

## Tables are column families

A **table** is a named part of the index: keys in one table are ordered among
themselves and are invisible to every other table. Two tables may hold the same
key without collision.

A table is a RocksDB **column family**. The alternative — one column family
with a prefix on every key — was rejected because:

- **Dropping is cheap.** Deleting a table drops the column family, and the data
  goes with it. Deleting a prefix means writing a tombstone over a range and
  waiting for compaction to reclaim the space.
- **Tuning is per table.** Compression, block size and bloom filters are column
  family options. A table of small hot records and a table of large cold ones
  want different settings, and prefixes cannot express that.
- **Ranges stay honest.** With prefixes, a scan of the whole table is a scan
  bounded by the next prefix, and one badly chosen key can walk into a
  neighbour's data. With column families the boundary is structural.

The cost is real and is the reason tables are coarse: each column family carries
its own memtable, so an idle table still holds memory, and hundreds of tables
are hundreds of memtables competing for the write buffer. **Tables are dozens,
not millions.** A table per customer is wrong; a customer prefix inside a table
is right.

## Keys are bytes, and ordering is byte-wise

RocksDB orders keys by unsigned byte comparison, and this API does not offer
another comparator. That is deliberate: the ordering is the whole contract of a
range scan, and a comparator that sorted `"10"` before `"9"` in one table and
not another would make every scan a question about which table it was reading.

The consequence belongs to whoever designs the keys:

- Numbers sort as text unless they are written fixed-width and zero-padded, or
  big-endian binary.
- Timestamps sort correctly as ISO-8601 in UTC, or as big-endian epoch
  milliseconds. Newest-first needs a descending scan, not an inverted key.
- Composite keys work by concatenating parts with a separator that cannot occur
  inside a part — a zero byte is the usual choice.

Keys travel in URLs, so the API has to say how bytes become text. See
[key encoding](/database/records#key-encoding): text keys are percent-encoded in
the path, binary keys are base64url with a header that says so.

## Values are opaque, and typed by the table

A value is a byte string the service never parses. The table declares one
`contentType` when it is created, and that is the type every record in it is
written and read as. A JSON table returns `application/json`; a table of images
or protobuf declares `application/octet-stream` and the client knows what it
put there.

Typing the table rather than the record is what keeps a read to one RocksDB
lookup. Per-record content types would mean storing a header beside every value
and parsing it on the way out, for a distinction almost no table actually makes.

## Open questions

- **Authentication.** The API is specified as reachable only from inside the
  network that owns it. A bearer token per client is the obvious next step, and
  nothing in the design turns on it.
- **Replication.** One instance, no failover. The durability a client asks for
  on a write ([durability](/database/records#durability)) is the only guarantee
  offered, and it is a guarantee about one disk.
- **Table TTL.** Per-table expiry is proposed, and the exact semantics need
  confirming against RocksDB's TTL support before it is written down: expired
  records are removed by compaction, so a TTL bounds space rather than
  guaranteeing that a record has become unreadable.
