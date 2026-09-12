# Scans

Reading records in key order, **one partition at a time**. This is what an
ordered store is for, and it is the part of the API worth designing carefully.

```http
GET /table/transaction/key?key=4821&limit=100
```

```json
{
  "records": [
    { "key": "4821", "sort": "2026-01-31", "value": "..." },
    { "key": "4821", "sort": "2026-02-28", "value": "..." }
  ],
  "next": "eyJrIjoiNDgyMVwwMDAwMjAyNi0wMi0yOCIsInMiOjQyMTk5LCJwIjo3fQ=="
}
```

`records` is in key order within the partition. `next` is a cursor, and its
absence means the range is exhausted.

## A scan names its partition

**Every scan reads one of the 256 [partitions](/database/cluster#one-copy-in-every-zone),
and says which.** A key belongs to one partition, the store holds each partition
apart from the others, and one node of a zone holds each — so a scan of a
partition is one range of one node's store, answered without asking any other
node. There is no scan of a whole table: that is a scan of each partition, and
it is the client that walks them.

| Parameter | Names the partition |
| --- | --- |
| `key` | The one holding this partition key. Hashing is the service's, so this is how a client asks for the records of a key it knows |
| `partition` | By number, `0` to `255`. This is how a client walks a whole table |

Exactly one of them, or the scan is
[`invalid_partition`](/database/reference#error-codes). `key` is the usual one:
`key=4821` is the partition holding `4821`, and the range parameters below then
bound the records inside it.

Walking a whole table is 256 scans, each of them independent and each answered
by whichever node holds that partition:

```http
GET /table/transaction/key?partition=0
GET /table/transaction/key?partition=1
...
GET /table/transaction/key?partition=255
```

They can be walked in any order, and several at a time — a client that wants a
table read quickly asks several partitions at once, and the requests land on
different nodes. What no client can ask for is the table serialised into one
ordered stream: that would be every node's share merged by whichever node was
asked, and a page of it costs every node a page.

A record with a [sort key](/database/records#partition-keys-and-sort-keys)
carries its two halves as two fields, which is what a client puts back into a
path. A record of one part has no `sort` at all:

```json
{
  "records": [
    { "key": "4821", "value": "the account" }
  ]
}
```

A scan is the one place a value travels inside a document rather than as the
body, so it appears as what it is: a JSON string. Keys and values are strings
and [every string is valid](/database/records#keys-and-values), so a value that
is itself a JSON document arrives here escaped, and is the client's to parse.

## The range

| Parameter | Means |
| --- | --- |
| `prefix` | Every key beginning with this. Shorthand for a `from`/`to` pair |
| `from` | Start here, **inclusive** |
| `to` | Stop here, **exclusive** |
| `reverse` | `true` walks the range from `to` back towards `from` |
| `limit` | At most this many records. Default 100, maximum 1000 — **and a page may be shorter**, see below |
| `values` | `false` returns keys only |

**The bounds range over the whole key**, both halves and the zero byte between
them, because that is the order the store holds — and they range *inside the
partition named*, which is the only place those keys are. A partition key sorts
below every key under it, so within `key=4821` the bounds `prefix=4821` are the
record `4821` and everything sorting under it.

`48210` is a different partition key, so it is in a different partition and no
bound reaches it from here: what used to need a bound below the byte after the
separator now needs nothing, because the partition is the fence.

```http
GET /table/transaction/key?key=4821&prefix=4821
```

`from` inclusive and `to` exclusive is RocksDB's own convention, and it is the
one that makes ranges compose: the `to` of one page is the `from` of the next
with nothing dropped and nothing repeated.

Omit both bounds and the scan is the whole partition, which is what a walk of a
table asks for 256 times.

**`limit` is a maximum and not a promise.** A page is bounded in bytes as well as
in records — 8 MiB of keys and values — because a limit of a thousand says
nothing about the size of a thousand records, and a value may be 16 MiB. A page
that reaches the byte budget first ends there and carries a cursor, so a table of
large values is paged rather than answered with a response the node would have to
build in memory before it could send any of it. **Follow the cursor until there
is none**, which is what a client should do anyway: a short page is not an
exhausted range, and only the absence of a cursor is.

A single record larger than the whole budget is still returned, alone, because a
scan that could not carry the record in front of it would never get past that
key.

`values=false` is not a cosmetic saving. It lets the service iterate without
fetching values, which for a table of large values is the difference between
reading index blocks and reading the table.

With `reverse=true` the bounds keep their meaning — `from` is still the low key
and `to` still the high one — and only the direction of travel changes. This is
worth stating because the alternative, swapping the bounds' meaning with the
direction, is how clients end up scanning an empty range and believing the table
is empty.

## Paging, and what a cursor promises

A cursor encodes the last key returned. Passing it resumes strictly after that
key:

```http
GET /table/transaction/key?key=4821&cursor=eyJrIjoiNDgyMVwwMDAwMjAyNi0wMi0yOCIsInMiOjQyMTk5LCJwIjo3fQ==
```

The cursor is opaque. It is not a key, and a client that decodes one and builds
its own has built a `from`, which it could have asked for honestly.

**A cursor belongs to one partition and to the instance that issued it**, and
both are checked: it carries the partition it was issued for, so a cursor given
back against another partition is
[`invalid_cursor`](/database/reference#error-codes) rather than an empty page.
Since the node answering is the one that holds the partition, it is also the
node that reads the cursor back, whichever node the client happens to ask.

**A paged scan is not a consistent read.** Each page is a new RocksDB iterator,
and an iterator sees the instance as it was when it was created. Records written
between two pages are visible to the second page and not the first. So:

- A record inserted behind the cursor is missed.
- A record inserted ahead of it appears, even though it did not exist when the
  scan began.
- A record overwritten ahead of the cursor is answered with its new value,
  though an earlier page would have answered the old one.

Within one page the view is consistent, because one iterator serves it. Across
pages it is not, and the API offers nothing that makes it so. A client that
needs a stable view of a range has to get it from the keys — scanning a range
that is no longer written to, such as a closed day or a finished run of work —
rather than from the scan.

## Scans and the shape of keys

The API cannot filter on anything but the key, so what a scan can answer is
decided when the keys are designed, not when the query is written. Two rules
carry most of it:

- **Put in the key, in order, what you will want to scan by.** A transaction
  with partition key `{account}` and sort key `{timestamp}` answers "this
  account's transactions, newest first" with one reverse scan of one partition,
  on [one node](/database/cluster#which-node-owns-a-key). The other way round —
  the timestamp as the partition key — answers that question only by walking
  every partition of the table and throwing most of it away.
- **A prefix scan is only cheap if the prefix is a prefix.** Asking for keys
  *containing* something is a full scan with the service throwing most of it
  away, which is why the API does not offer it: it would look like a query and
  cost like a table scan.

Where both orders are genuinely needed, the answer is a second table holding the
other key order, written by the client as it writes the first. That is a
secondary index, built explicitly, with its cost — including the window in which
the two tables disagree — visible at the point where it is paid.
