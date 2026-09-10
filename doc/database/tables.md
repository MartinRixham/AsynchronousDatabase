# Tables

A table is a named keyspace, and a RocksDB column family. Names are `[A-Za-z0-9_ -]`,
between 1 and 64 characters. `default` is reserved for the column family RocksDB
always has.

A table also declares the tables it is derived from, as
[`dependencies`](#dependencies). The tables and their dependencies are the graph
the UI draws, and the order in which a processor has to fill them.

A table declares whether it is [`immutable`](#immutable-tables). A key of an
immutable table is written once and never again, and nothing it holds is
deleted.

## List the tables

```http
GET /table
```

```json
{
  "tables": [
    { "name": "account", "dependencies": [], "immutable": false },
    { "name": "transaction", "dependencies": [ "account" ], "immutable": false }
  ]
}
```

Instances hold dozens of tables, so the list is not paged.

## Create a table

```http
PUT /table/transaction
Content-Type: application/json

{
  "dependencies": [ "account" ]
}
```

`201 Created` when the table is new, `200 OK` when it already existed with these
options, `409 Conflict` when it existed with different ones. Creating a table is
therefore safe to run at every start-up, which is how a service should declare
the tables it needs.

The body is a JSON object of the options below, and an empty body is the
defaults. A body that is not a JSON object — malformed, or a list or a number —
is `400 invalid_body`. This is the one body the service parses: a
[value](/database/records#keys-and-values) is never looked at.

| Option | Default | Means |
| --- | --- | --- |
| `dependencies` | `[]` | The names of the tables this one is derived from. See [dependencies](#dependencies) |
| `immutable` | `false` | Each key of this table may be written once. See [immutable tables](#immutable-tables) |

An option that is not of the type in the table — `immutable` as a string, say —
is `400 invalid_body`.

## Dependencies

`dependencies` is the list of tables this table is derived from — the tables
that have to be written before this one can be. It is a list of names, and
nothing else: the API records the edge, it does not run the work.

**Every name in the list must already be a table.** A create that names one
that is not is `400 dependency_not_found`, so the graph never holds an edge to
a table that does not exist. The order the tables are created in is therefore
the order the edges point in, and a cycle cannot be built out of edges that
only ever point at what is already there.

An empty list — the default — is a table nothing feeds, which is where a
consumer starts reading the graph and where the UI puts the first row.

## Immutable tables

```http
PUT /table/event
Content-Type: application/json

{
  "immutable": true
}
```

**A key of an immutable table is written once, and what the table holds it
keeps.** A `PUT` to a key the table already holds is `409 record_exists`, and
deleting a record is `409 table_immutable` — the value that is there is the
value that stays there, and a request that is refused changes nothing, on any
node.

Both halves are needed for the one promise. A key that could be deleted could
be written again, so a table that refused the overwrite alone would be one where
writing a key twice took two requests rather than one.

| Request | On an immutable table |
| --- | --- |
| `PUT /table/{table}/key/{key}`, a key it does not hold | `204`, written like any other |
| `PUT /table/{table}/key/{key}`, a key it holds | `409 record_exists` |
| `DELETE /table/{table}/key/{key}` | `409 table_immutable` |
| `DELETE /table/{table}/key?…`, a [range](#delete-a-range) | `409 table_immutable` |
| `DELETE /table/{table}` | `204`, and the data goes with the column family |

So an immutable table only grows, and the one thing that removes anything from
it is [dropping the table](#delete-a-table) — which drops all of it, and is the
only cheap way to delete a lot of data anyway.

Three details that follow from it:

- **The empty string is a value**, so a key written empty is a key the table
  holds, and writing it again is refused.
- **A delete is refused whether or not the key is there.** Deleting a record
  that does not exist is `204` everywhere else; here the answer is about the
  table, and no node has to be asked about the key to give it.
- **Each table answers for itself.** The same key of two tables is two records,
  and only the immutable one refuses.

The flag is an option like any other, so it is fixed when the table is created:
declaring the table again with a different `immutable` is `409 table_exists`,
the same as declaring it with different dependencies. Turning it on for a table
that exists means dropping the table and creating it again, which drops its data
with it.

In a cluster a refused overwrite is the answer of the node that
[leads the key's partition](/database/cluster#one-leader-for-each-partition),
which holds a copy of the key and is the one node ordering writes to it. It
answers before any copy is written, so a refused write leaves every copy as it
was. A refused delete needs neither the key nor a leader: every node holds the
table document, so the node the request landed on answers it without a hop.

None of this reaches what the cluster does with the records itself. A record
still moves to the node that owns it when the membership changes, and the node
that gave it up still clears down the copy it no longer owns — that is
[a share of a table moving](/database/cluster#moving-a-share-of-a-table) and not
a client deleting a record.

## Inspect a table

```http
GET /table/account
```

```json
{
  "name": "account",
  "dependencies": [],
  "immutable": false
}
```

## Delete a table

```http
DELETE /table/account
```

`204 No Content`, and the data is gone with the column family — no tombstones,
no wait for compaction. `404 Not Found` if there was no such table. An
[immutable](#immutable-tables) table is dropped like any other: it is its
records that it keeps, and dropping it takes all of them at once.

This is the only cheap way to delete a lot of data. Deleting the records of a
table one by one, or over a range, leaves tombstones behind
([delete a range](#delete-a-range)).

### Delete a range

```http
DELETE /table/account/key?prefix=user:2019
```

Deletes every record in the range in one operation, as a RocksDB range
tombstone rather than a delete per key. `204 No Content`.

Two one to know before using it:

- Range tombstones make reads that cross them slower, because every read in the
  range has to consult the tombstone. A table that accumulates many of them
  wants a compaction, or wants to have been a table that could be dropped
  whole.

It takes the same range parameters as a [scan](/database/scans#the-range), and
refuses a request that names no range at all — deleting every record in a table
is `DELETE /table/{table}` and then creating it again.

An [immutable](#immutable-tables) table refuses it outright, with
`409 table_immutable`.
