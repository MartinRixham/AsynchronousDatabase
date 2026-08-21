# Scans

Reading records in key order. This is what an ordered store is for, and it is
the part of the API worth designing carefully.

```http
GET /table/account/key?prefix=user:&limit=100
```

```json
{
  "records": [
    { "key": "user:4821", "value": "Eleanor Whitmore" },
    { "key": "user:7203", "value": "Marcus Hale" }
  ],
  "next": "eyJrIjoidXNlcjo3MjAzIiwicyI6NDIxOTl9"
}
```

`records` is in key order. `next` is a cursor, and its absence means the range
is exhausted.

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
| `limit` | At most this many records. Default 100, maximum 1000 |
| `values` | `false` returns keys only |

`from` inclusive and `to` exclusive is RocksDB's own convention, and it is the
one that makes ranges compose: the `to` of one page is the `from` of the next
with nothing dropped and nothing repeated.

Omit both bounds and the scan is the whole table, which is a legitimate thing to
ask for and an expensive one.

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
GET /table/account/key?prefix=user:&cursor=eyJrIjoidXNlcjo3MjAzIiwicyI6NDIxOTl9
```

The cursor is opaque. It is not a key, and a client that decodes one and builds
its own has built a `from`, which it could have asked for honestly.

**A paged scan is not a consistent read.** Each page is a new RocksDB iterator,
and an iterator sees the instance as it was when it was created. Records written
between two pages are visible to the second page and not the first. So:

- A record inserted behind the cursor is missed.
- A record inserted ahead of it appears, even though it did not exist when the
  scan began.
- A record deleted ahead of the cursor is gone, though an earlier page might
  have promised it.

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
  keyed `{account}\0{timestamp}` answers "this account's transactions, newest
  first" with one reverse scan. Keyed `{timestamp}\0{account}` it answers "every
  account's transactions in time order" instead, and answers the first question
  only by reading everything.
- **A prefix scan is only cheap if the prefix is a prefix.** Asking for keys
  *containing* something is a full scan with the service throwing most of it
  away, which is why the API does not offer it: it would look like a query and
  cost like a table scan.

Where both orders are genuinely needed, the answer is a second table holding the
other key order, written by the client as it writes the first. That is a
secondary index, built explicitly, with its cost — including the window in which
the two tables disagree — visible at the point where it is paid.
