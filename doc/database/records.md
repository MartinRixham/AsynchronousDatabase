# Records

One key, one value. These are the four operations most clients will ever use.

## Key encoding

RocksDB keys are arbitrary bytes; URL paths are text. The API takes text keys as
the normal case and binary keys as the exception.

**Text keys** are UTF-8, percent-encoded in the path. `/` must be encoded as
`%2F`, because the path segment ends at an unencoded slash:

```http
GET /table/account/key/user%3A4821
```

**Binary keys** are base64url, declared by a header that applies to the whole
request — path, query parameters and any keys in the response body:

```http
GET /table/event/key/AAABi9k5wAA HTTP/1.1
X-Key-Encoding: base64url
```

The default is `X-Key-Encoding: text`. A key that is not valid UTF-8 cannot be
sent as text, and a request that tries returns `400 invalid_key_encoding` rather
than storing something the client did not mean.

Keys are limited to 4 KiB, values to 16 MiB. An empty key is not a key; an empty
value is a legitimate value, and is how a table is used as a set.

## Read a record

```http
GET /table/account/key/4821
```

```http
200 OK
Content-Type: application/json
ETag: "9f8a1c4d5e6b7a20"

{"firstName":"Eleanor","lastName":"Whitmore"}
```

The body is the stored bytes, unchanged. The `Content-Type` is the table's
declared type, not something stored per record.

`404 Not Found` when there is no such key, with no body. A missing key and an
empty value are different things, and the status code is what tells them apart.

`HEAD` answers the same headers with no body, and is the cheap way to ask
whether a key exists and how large it is.

### ETags

The `ETag` is a 64-bit hash of the value bytes. It is what
[conditional writes](#conditional-writes) compare against, and it makes
`If-None-Match` on a read work as an ordinary cache validator:

```http
GET /table/account/key/4821
If-None-Match: "9f8a1c4d5e6b7a20"
```

`304 Not Modified` if the value still hashes the same. Note what this does *not*
say: two identical values written at different times share an ETag, so an ETag
is a statement about the value, not about when it was written. A client that
needs to know a record was rewritten with the same content has to put that in
the value.

## Write a record

```http
PUT /table/account/key/4821
Content-Type: application/json

{"firstName":"Eleanor","lastName":"Whitmore"}
```

`204 No Content`. The `Content-Type` must match the table's declared type, or
the request is refused with `415 Unsupported Media Type` — a table of JSON
should not quietly accumulate records that are not JSON.

An unconditional `PUT` returns `204` whether or not the key existed, and does
**not** return `201`. Telling the two apart would mean reading the key before
writing it, which doubles the cost of the commonest operation in the API to
report something almost no client acts on. A client that does care asks for it,
with `If-None-Match: *`.

There is no `POST` to a table and no server-generated key. Keys carry meaning in
an ordered store — they decide what a scan can answer — so the API will not
invent one.

## Delete a record

```http
DELETE /table/account/key/4821
```

`204 No Content`, whether or not the key existed. Deleting a key that is not
there is a no-op in RocksDB, and reporting `404` would mean a read before every
delete for the same reason as above. `DELETE` is idempotent either way.

## Conditional writes

A conditional `PUT` or `DELETE` runs inside a RocksDB transaction that takes the
key for update, so the check and the write cannot be separated by another
client's write.

| Header | Means | Fails with |
| --- | --- | --- |
| `If-None-Match: *` | Write only if the key does not exist | `412 Precondition Failed` |
| `If-Match: "<etag>"` | Write only if the current value has this ETag | `412 Precondition Failed` |
| `If-Match: *` | Write only if the key exists | `412 Precondition Failed` |

`If-None-Match: *` returns `201 Created` when it succeeds, because in that case
the API has already paid to know the key was absent.

This is the compare-and-swap the API offers, and it is enough to build a
counter, a lock or an optimistic update loop. It is not enough to make two
records consistent with each other: every write stands alone, so a client that
needs two keys to change together has to live with the window between them. A
multi-key write is the one thing a client may reasonably want that this API does
not have; if it turns out to be needed, it is a resource of its own, not another
header.

## Durability

Every write takes a durability level, as a header, defaulting to `wal`:

```http
PUT /table/account/key/4821
Durability: sync
```

| Level | RocksDB | Survives | Costs |
| --- | --- | --- | --- |
| `none` | `disableWAL` | Nothing. The write is in the memtable and is lost if the process dies | Nothing |
| `wal` | default | The process dying. Not the machine losing power | A write to the OS |
| `sync` | `sync = true` | The machine losing power | An fsync, so hundreds of microseconds to milliseconds |

The default is `wal` because it is the level at which the API's failure mode
matches the client's expectation: a `204` means the record is in the log, and a
crash of this service does not lose it. `sync` is for the writes a customer
would notice losing — money moving, an application accepted. `none` is for bulk
loading a table that would simply be loaded again.

The level is per write, not per table, because the same table usually holds both
kinds of write.
