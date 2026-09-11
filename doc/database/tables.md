# Tables

A table is a named keyspace, and a RocksDB column family. Names are `[A-Za-z0-9_ -]`,
between 1 and 64 characters. `default` is reserved for the column family RocksDB
always has.

A table also declares the tables it is derived from, as
[`dependencies`](#dependencies). The tables and their dependencies are the graph
the UI draws, and the order in which a processor has to fill them.

## List the tables

```http
GET /table
```

```json
{
  "tables": [
    { "name": "account", "dependencies": [] },
    { "name": "transaction", "dependencies": [ "account" ] }
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

## Inspect a table

```http
GET /table/account
```

```json
{
  "name": "account",
  "dependencies": []
}
```

## Delete a table

```http
DELETE /table/account
```

`204 No Content`, and the data is gone with the column family — no tombstones,
no wait for compaction. `404 Not Found` if there was no such table.

**This is the only way to erase a record.** There is no delete of a key and no
delete of a range: both are `405 method_not_allowed`, and a table is dropped
whole or not at all. A record that should no longer be read is
[overwritten](/database/records#there-is-no-way-to-erase-one-key), and a table
whose records are meant to go away is one that can be dropped and created again.

What that buys is a store with no tombstones in it: a delete per key, or a range
tombstone over many of them, is read again by every scan that crosses it until a
compaction takes it away, and there is no key here that a read has to consult a
tombstone for.
