# Tables

A table is a named keyspace, and a RocksDB column family. Names are `[a-z0-9_-]`,
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

This is the only cheap way to delete a lot of data. Deleting the records of a
table one by one, or over a range, leaves tombstones behind
([delete a range](#delete-a-range)).

## Maintenance

Both of these are operator actions. They are in the API because they are
sometimes the answer to a performance problem, not because a client should call
them in normal use.

### Delete a range

```http
DELETE /table/account/key?prefix=user:2019
```

Deletes every record in the range in one operation, as a RocksDB range
tombstone rather than a delete per key. `204 No Content`.

Two things to know before using it:

- Range tombstones make reads that cross them slower, because every read in the
  range has to consult the tombstone. A table that accumulates many of them
  wants a compaction, or wants to have been a table that could be dropped
  whole.

It takes the same range parameters as a [scan](/database/scans#the-range), and
refuses a request that names no range at all — deleting every record in a table
is `DELETE /table/{table}` and then creating it again.
