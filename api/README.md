# Postman

The key journeys of the API, as an importable Postman collection with assertions on every response.
What it asserts comes from [`doc/database`](../doc/database), which is the spec: the documented
status codes, the documented error codes, and the behaviour those documents promise.

| File | Is |
| --- | --- |
| `asyncdb.postman_collection.json` | The collection: eight folders, run in order |
| `asyncdb.local.postman_environment.json` | One instance answering on the API port, `localhost:8080` |
| `asyncdb.compose.postman_environment.json` | The three instances of `docker-compose`, through the nginx in front of each |
| `asyncdb.aws.postman_environment.json` | The three instances of the AWS stack, behind the one load balancer in front of them all |

## Import

In Postman, **Import** and select all four files, then pick an environment top right. Everything
else — the table names, the keys, the cursor a scan issues — is a collection variable, so nothing
has to be edited to run it.

## Run it

Against a single instance built from this repo:

```bash
cmk run                                     # serves on 8080, no cluster
newman run api/asyncdb.postman_collection.json -e api/asyncdb.local.postman_environment.json
```

Against the whole thing, cluster and all:

```bash
docker-compose up -d
newman run api/asyncdb.postman_collection.json -e api/asyncdb.compose.postman_environment.json
```

Against the deployed stack, whose address is the `Url` output of the CloudFormation stack. Every node
answers for every key, so the one load balancer stands for all three, and the URL is the only thing
the environment needs told:

```bash
URL=$(aws cloudformation describe-stacks --stack-name asyncdb \
  --query "Stacks[0].Outputs[?OutputKey=='Url'].OutputValue" --output text)
newman run api/asyncdb.postman_collection.json -e api/asyncdb.aws.postman_environment.json \
  --env-var baseUrl=$URL/asyncdb --env-var node1=$URL/asyncdb \
  --env-var node2=$URL/asyncdb --env-var node3=$URL/asyncdb
```

This is what the build does on a push to `master`: it creates the stack, waits for `/health` to name
three nodes, runs the collection, and deletes the stack whether the collection passed or not — a
failing assertion fails the build.

The requests depend on the ones before them — a scan reads what the writes before it seeded, and
page two of a scan carries the cursor page one issued — so run a folder whole, and run the folders
in order. Folder 0 drops what a previous run left, which is what makes a re-run start from a create
that is a create.

## The journeys

| Folder | The journey |
| --- | --- |
| 0 · Reset | Drop the tables a previous run left, so a re-run starts clean |
| 1 · Health | Liveness, `write_stalled`, the nodes this instance sees, and a method it refuses |
| 2 · Declare the table graph | What a service does at every start up: create, create again (200), create with different options (409), a dependency that is not a table (400), a name holding a character a table name may not (400), the reserved name `default` (400), a body that is not a JSON object (400), then inspect and list |
| 3 · The record lifecycle | Write, read, `HEAD`, overwrite, a JSON value returned byte for byte, the empty value against the missing key, delete, and delete again |
| 4 · Keys the API must not mangle | A space and a `é`, an encoded slash, a key that is not valid UTF-8 (400), a key over 4 KiB (413) |
| 5 · Scans | Prefix, `values=false`, `from` inclusive against `to` exclusive, `reverse`, two pages and the cursor between them, a foreign cursor (400), an inverted range (400), the whole table |
| 6 · Delete a range | A range delete with no range (400), a prefix deleted whole, and a key outside it that survived |
| 7 · Drop the tables | `204`, then `404`, then created again and empty — the data went with the column family |
| 8 · The cluster | Three instances: the membership, a table created on one node and present on all, a record written to one node and read from the others, a scan merged across all three, and a range delete and a table delete that reach every node |

## Three things worth knowing before a red test is believed

- **The compose cluster keeps two copies of a record.** `docker-compose.yml` puts nodes 1 and 2 in
  one zone and node 3 in another, so every record is on node 3 and on one of the other two — see
  `doc/database/cluster.md`. Nothing in the collection asserts which node holds a key, so this
  changes no assertion — but a record read from a node that "should not" have it is the cluster
  working rather than a red test.
- **One assertion in folder 8 needs a cluster.** `clusterSize` is what tells the collection which
  it is running against. Set to 1 it asserts instead that `/health` names no membership at all,
  which is what standing alone looks like, and the rest of the folder runs as it stands: `node1`,
  `node2` and `node3` are then the one instance, and each request is answered where it lands rather
  than forwarded. Nothing else in the collection is conditional, so every environment runs the whole
  thing and every one of them comes back green.
- **Table names are wider than `doc/database/reference.md` says.** The server takes letters of
  either case, digits, spaces, `_` and `-`, so the collection asserts `invalid_table_name` on a name
  holding a `.` rather than on a capital. The reference still says `[a-z0-9_-]`.

An encoded slash in a key used to be one of these — nginx's `rewrite` worked on the decoded path, so
`%2F` reached the server as a real slash and the request was a different route. `server/server.conf`
now strips `/asyncdb` from `$request_uri` instead, which is the target as it was sent, so the two
requests that write and read such a key are exercised through the proxy as well.
