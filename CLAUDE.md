# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

A C++20 HTTP/JSON database server for asynchronous data processing (Boost.Beast + RocksDB), plus a
vanilla-JS single-page UI that draws the tables and their dependencies as a DAG. Several instances
partition a keyspace between them and keep one copy of it in each availability zone, finding each
other through etcd — see [Clustering](#clustering).

## Build and test

The C++ side is built with [Cheesemake](https://github.com/martinrixham/cheesemake), driven by
`recipe.json`. `cmk` runs **all** phases up to the one named, in order:
`validate` → `compile` → `test` → `package` → `verify` → `run`.

```bash
cmk verify          # full build: cppcheck, compile, run tests, link build/bin/asyncdb
cmk test            # stop after running tests
cmk clean verify    # wipe build/ first (incremental builds are hash-based, see build/hashes)
cmk run             # build then run build/bin/asyncdb
```

All test sources link into the single gtest binary `build/test/test_main`, so a single test or suite is
run directly after `cmk test`:

```bash
build/test/test_main --gtest_filter='router_test.*'
build/test/test_main --gtest_filter='table_test.fail_to_deserialise_table_with_no_name'
```

Notes:
- `-Wall -Werror` — any warning fails the build. `cppcheck --enable=style` runs in the `validate` phase.
- **Incremental builds hash sources, not headers.** Changing a header does not rebuild the objects
  that include it, so a change to a struct or a class layout leaves stale objects that link and then
  corrupt memory at run time. `cmk clean test` after touching anything under `src/**/*.h`.
- The `verify` phase memchecks under valgrind, which is why `cmk verify` takes minutes rather than seconds.
  Cheesemake's own `valgrind.chevre` runs `build/bin/asyncdb`, and that serves until it is signalled, so the
  root `valgrind.chevre` overrides it and runs `build/test/test_main` instead, keeping the report in
  `build/test/test_main.valgrind`. **A definite or possible leak fails the build**, and the suite is clean of
  both. It is the plugin's own grep of the leak summary that fails it, and not `--error-exitcode=1`: that
  would fail on every error valgrind reports, still reachable included, and the release image builds on musl,
  where a block libstdc++ or RocksDB still holds at exit would fail the build untested.
- **A test that starts a server has to stop it.** `serve()` returns only when the acceptor is closed, and an
  always-pending accept holds a `shared_ptr` to the server, so a detached serving thread leaks the server,
  its thread pool and its RocksDB. `server_test` closes and joins in `TearDown`.
- `compile_flags.txt` is for clangd only; the real flags come from `recipe.json`.
- Formatting is enforced by `.clang-format` (tabs, Allman braces, 120 columns, `SortIncludes: false`).
- Naming is `snake_case` throughout, including class names, and each layer lives in its own namespace
  matching its directory.

### UI (`ui/`)

```bash
cd ui
npm test            # eslint + vitest (this is what CI/Docker runs)
npm run test:watch  # vitest watch
npm start           # vite dev server
npm run build       # emits ui/dist, served by nginx in the image
```

Vitest only picks up files matching `test/**/*Test.js`.

### Wiki (`doc/`) and API collection (`api/`)

`doc/` is the VitePress site that *is* the API spec — `cd doc && npm run dev` to read it, `npm run
build` to render it. `doc/database/` is the API; `doc/deployment/` is the AWS stack. Neither CI nor
the image builds it. `api/` is a Postman collection whose assertions come from that spec, and it runs
headless against a server that is already up (see `api/README.md`):

```bash
cmk run   # serves on 8080 with no cluster; or docker-compose up -d and use the compose environment
newman run api/asyncdb.postman_collection.json -e api/asyncdb.local.postman_environment.json
```

The eight folders are ordered and depend on each other — folder 0 drops what the last run left, and
a scan's second page carries the cursor its first page issued — so run whole folders, in order. The
`clusterSize` variable is how many nodes folder 8 expects `/health` to name: 3 for compose, 6 for the
AWS stack, 1 for a lone instance.

### Browser tests (`automation/`)

Playwright, end to end over the UI in Chromium, and no part of `cmk`:

```bash
podman-compose up -d                                              # or docker-compose
cd automation && npm install && npx playwright install chromium   # once
npm test
```

**The tests start nothing.** They drive an instance that is already running, at `ASYNCDB_URL`
(default `http://localhost:8080`, the first node of compose) — one address for the whole thing,
because the nginx in front of each instance serves the UI and proxies `/asyncdb` to the database.
The `Url` output of the CloudFormation stack is the other one, which is what `build.yaml` passes
after the Postman collection and before it tears the stack down. `globalSetup.js` waits up to a
minute for `/asyncdb/health` and stops the run with that address if nothing answers.

There is no stub: `Database.js` seeds the tables a journey starts on over the real API and reads
back what the page wrote. **Each test drops every table it finds, before and after** — the graph the
page draws is every table the instance holds — which is why the suite runs `workers: 1` and why
`ASYNCDB_URL` must name an instance whose tables can go. Specs match `*Test.js`, as in `ui/test`.

### Load tests (`perf/`)

`perf/read.sh` and `perf/write.sh` share `perf/harness.sh`, which forks `THREADS` curl workers over
persistent connections and reports latency percentiles. Everything is an environment variable:
`THREADS`, `REQUESTS`, `BASE` (default `http://localhost:8080/asyncdb`), plus `URL` for reads and
`TABLE` and `VALUE_BYTES` for writes. They drive a server that is already running and are
no part of `cmk`.

**They are tests as well as measurements.** A request the server answers with anything but a 2xx —
including the `000` of a transfer that never answered — makes the run exit non-zero, which is what
lets `build.yaml` run them last against the deployed stack and fail the build on them. Latency is
reported and never asserted on: nothing here is a threshold.

### Running the whole thing

`docker-compose up`, then the UI is on `localhost:8080`. The image runs nginx on port 80 serving
`ui/dist` and reverse-proxying `/asyncdb/*` to the `asyncdb` binary on `localhost:8080`
(`server/server.conf`); that is why `DatabaseClient` uses relative `asyncdb/...` URLs. The compose
file brings up **three** instances (`localhost:8080`, `8081`, `8082`) and the etcd they partition
their keyspace through, so any of them answers for every key. They are in **two** zones — 1 and 2 in
`one`, 3 in `two` — so the compose cluster partitions inside zone `one` and keeps a whole copy in
zone `two`.

### Clustering

`ASYNCDB_ETCD` (where etcd answers — one base URL, or every member of the etcd cluster separated by
commas, tried in turn and sticky on whichever answered), `ASYNCDB_NODE` (this node as the others
reach it, the API port and not the nginx in front of it) and `ASYNCDB_ZONE` (the availability zone
this node is in). **Set none and nothing changes**: no thread is started, nothing is registered, and
the instance owns the whole keyspace, which is what every test that is not `cluster_test` runs as.
Set the first two and the instance joins.

`ASYNCDB_ZONE` is what turns partitioning into replication, and it is the only knob there is:
**a key belongs to one of 256 partitions, the membership is grouped by zone, and the partition is
hashed once inside each group**, so every zone holds exactly one copy of every partition — and the
copies of a partition are the same three nodes for every key in it, which is what lets one of them
lead it. No zone named anywhere is one zone holding all the nodes, which
is the one copy this always kept. `docker-compose.yml` runs two zones (nodes 1 and 2 in `one`,
node 3 in `two`) so the compose cluster both partitions and replicates; `cloudformation.json` reads
the real AZ out of IMDS, which is three zones of one node each.

## Libraries

The server is Boost, RocksDB and libcurl; the tests add gtest and gmock. `recipe.json` lists
`rocksdb`, `libcurl`, `gtest` and `gmock` — and **nothing for Boost**, because every Boost header used
here is header-only.

**That is a rule, not an accident.** The runtime image installs `libstdc++ rocksdb curl nginx` and no
Boost at all (`Dockerfile`), so reaching for a compiled Boost library costs a `recipe.json` dependency
*and* a new runtime package in the image. Boost.JSON is the one that would otherwise need linking, and
it is kept header-only by including `<boost/json/src.hpp>` exactly once per binary — in `src/main.cpp`
for the server and in `test/repository/rocksdb_repository_test.cpp` for the test binary. Do not add a
second one to either.

| Library | Used for |
| --- | --- |
| Boost.Beast + Boost.Asio | The HTTP server, the verbs and the status codes, all the way into `router::response` |
| Boost.JSON | Every document the API reads or writes, and the table document in RocksDB |
| Boost.Locale (`utf.hpp` only) | `record::is_valid_utf8` — the `utf_traits` decoder needs no linking, and rejects truncation, surrogates and overlongs |
| Boost.Algorithm | Splitting a path and a query string in `url` |
| Boost.LexicalCast | `try_lexical_convert` for the scan `limit`, instead of `stoul` in a `try` |
| RocksDB | The store. A table is a column family |
| libcurl | Percent-decoding in `url::encode`/`decode`, talking to etcd and to the other nodes, and driving the server in `server_test` |
| etcd | Membership only, over its **JSON gateway** (`POST /v3/kv/put`, `/v3/lease/grant`, …), so there is no gRPC dependency |

**Prefer a header-only Boost to writing it again** — that is what replaced a hand-rolled UTF-8 decoder
with `boost::locale::utf`. Two deliberate exceptions:

- **Boost.URL** would replace most of `url`, and it handles `%2F`, `%00` and invalid UTF-8 correctly.
  It is not used because `<boost/url/src.hpp>` is discontinued as of Boost 1.90 (it is an `#error`), so
  it would have to be linked; and because `params()` decodes `+` as a space, which would quietly
  corrupt any key or `from`/`to` bound containing a literal `+`.
- **base64** in `src/base64` — a scan cursor and every key and value etcd's JSON gateway carries — is
  written out rather than taken from `boost::beast::detail::base64`, which works but is Beast's
  private namespace.

On the other side, the UI's one dependency of substance is
[@datumjs/datum](https://www.npmjs.com/package/@datumjs/datum) — see [UI architecture](#ui-architecture)
— and the wiki in `doc/` is a VitePress site with its own `package.json`.

## Server architecture

The API the server implements is the one the wiki describes in `doc/database/` — that is the spec,
and `doc/database/reference.md` is the list of endpoints, error codes and limits.

Request flow, one layer per directory under `src/`:

`main.cpp` → `server::server` → `server::session` → `router::router` → `repository::repository` →
`table::table` / `record::record` / `scan::range`

and, off the router, `cluster::cluster` → `http::client` → the other nodes and etcd.

- **`server::server`** owns the `io_context`, the acceptor and a thread pool sized by
  `server::thread_pool_size()` — `ASYNCDB_THREADS`, defaulting to eight threads a core, bounded to
  between sixteen and a hundred and twenty-eight. **It is deliberately not `hardware_concurrency()`**: a thread here waits on
  another node for most of a forwarded request, so the pool is a count of requests that can be in
  flight rather than of cores, and two threads on a two core instance is a server that two waiting
  requests fill — health check included, which is what has the instance replaced. Each thread keeps
  its own curl handles, so the pool is also how many connections a node holds to each neighbour.
  It also owns the single `rocksdb_repository` and `router`, which are shared
  by reference across all sessions — anything reached from the router must be safe for concurrent use.
  Constructing with port `0` picks a free port and exposes it via `port()`; tests rely on this.
- **`server::session`** is one connection: async read → `handle_request()` → async write, looping while
  keep-alive. The server holds every live session weakly, because **closing the acceptor does not
  close the connections already made**: peers and the nginx upstream pool both keep theirs open, and
  a session waiting for a request that is not coming would hold `serve()` open until its 60 second
  timeout. `close()` therefore cuts the waiting sessions and lets the busy ones finish, answering
  them `Connection: close`. It rejects methods other than GET/HEAD/PUT/DELETE and any path segment that is `..`,
  builds a `router::request`, and turns an escaping `repository::storage_error` into its own status and
  any other exception into a 500 `storage_error`. HEAD answers the headers of the GET with no body and
  the length the body would have had.
- **`url`** splits the target at its unencoded slashes *before* percent-decoding each segment, so a key
  containing `/`, `?` or a zero byte stays one segment. Query values are decoded the same way.
- **`router::router`** matches routes by hand — `/health`, `/table`, `/table/{table}`,
  `/table/{table}/key` and `/table/{table}/key/{key}` — and returns a `router::response` (status,
  content type, and either a `boost::json::object` or the raw text of a value). `router/api_error.cpp`
  is the one place a documented error code is mapped to a status.
- **`http::client`** is the seam over libcurl, and `curl_client` keeps **one handle per thread**,
  reset before each request. The handle is what holds open connections, so a node that forwards to
  the same few neighbours stops paying for a handshake each time — and `curl_easy_reset` is what
  keeps the last request's body, or a HEAD's "no body", out of the next one. `send_all` is the same
  thing for a **fan out** — the copies of a record, or every node of a table create — run in one
  `curl_multi` handle per thread, so the thread waits for the slowest of them rather than for the
  sum of them, and the multi handle holds that fan out's connections the way the single handle
  holds its own. **A fan out does not copy the bodies it is given**, so the requests have to
  outlive the call, and a fan out of one runs on the single handle instead.
- **`cluster::cluster`** is the second pure-virtual seam the router routes against, over "which
  nodes hold this key" and "ask that node". `cluster::replicas` answers a `cluster::placement` —
  whether this node holds a copy, and the other nodes that do, this node's own zone first.
  `cluster::zones` groups the membership for a scan: the nodes of each zone, this node's own first,
  which is why a scan asks one zone rather than every node. `cluster::standalone` holds everything
  and is what a router built without a cluster gets;
  `cluster::etcd_cluster` registers `/asyncdb/node/{address}` in etcd on a lease with
  `{"node":...,"zone":...}` as its value (a bare address is still read, as a node in no zone),
  renews it on a thread of its own, and reads the membership back. `cluster::owner_of` is rendezvous
  hashing over a set of nodes, `cluster::owners_of` runs it once per zone, `cluster::zones_of` is the
  grouping behind `zones()`, and `cluster::forward` is how a request travels.
- **`repository::repository`** is the pure-virtual seam, over tables, records, scans and range deletes.
  `rocksdb_repository` makes each table a **column family** and keeps its document in the default one
  under `"TABLE_<name>"`; dropping a table drops the column family, so the data goes with it. The
  handle map is guarded by a `shared_mutex`.

### Domain conventions worth knowing

- `table::table`, `record::record` and `scan::range` all carry `bool is_valid` plus a `code` and a
  `message`. **Validation failures are values, not exceptions**: `table::invalid_table(code, msg)`
  returns a table the router turns into the status that code names and the repository silently refuses
  to persist. Follow this pattern rather than throwing; exceptions are reserved for genuine
  infrastructure failure — `repository::storage_error` carries `storage_error` or `write_stalled`, and
  `ERROR(...)` from `src/error.h` prefixes file/function/line.
- `table::parse_table` enforces the invariants: a name of 1–64 characters that is not `default`, and
  every dependency must name an existing table — so the dependency graph can never contain a
  dangling edge. `PUT /table/{table}` is idempotent: the same options again are `200`, and
  different ones are `409`, which is why a cycle cannot be built.
  **The accepted characters are `[A-Za-z0-9_ -]`** — either case, digits, space, underscore, hyphen —
  which `doc/database/`, `reference.md` and `is_valid_name`'s own error message all still describe as
  `[a-z0-9_-]`; `api/README.md` asserts against the wider set the code actually takes.
- `record::parse_record` enforces the limits (4 KiB of key, 16 MiB of value) and that a key is valid
  UTF-8. A value is never looked at — every string is a value, and the empty one is told from a missing
  key by the status code, which is why `read_record` returns a `std::optional`.
- `scan::range` is the parsed query of a scan or a range delete, and a cursor is base64 of
  `{ "k": last key, "s": instance }`; the instance is what makes a cursor this instance did not issue
  refusable.
- **Partitioning is by key alone, never by table**, so the same key of two tables is in one
  partition and a record and the records derived from it are one hop. A write is ordered by the node
  **leading** the key's partition, which writes the copy in every zone and every one of them has to
  take it — all of them at once, so a copy that refuses is a copy the others were written beside
  rather than ahead of; a read goes to one copy — this node when it
  holds one, else the nearest zone's, passing over a node that does not answer, and asking the other
  copies when this node holds nothing for the key. A table create or delete goes to *every* node,
  because a record can only be written where its table is; a scan is asked of **one zone** — this
  node's own, since a zone holds a copy of the whole keyspace — and merged back into key order,
  falling back to another zone when a node of that one does not answer. A forwarded request carries
  `X-Asyncdb-Forwarded` and is served where it lands, which is what stops two nodes bouncing it.
  `doc/database/cluster.md` is the spec, including what this deliberately does not do (no read
  repair, no rebalancing, no replication log, and a write that needs both a leader and every copy).
- **A leader is claimed in etcd, not elected by votes.** `/asyncdb/leader/{partition}` is written
  with a transaction that only succeeds if nothing created the key, on the node's own membership
  lease — so a node that stops renewing stops leading. A node claims
  `claims_per_refresh` (64) partitions per pass, from an offset of its own name, so a cold start is
  a pass or two rather than 256 round trips. The **term** is the etcd revision that
  created the claim; it travels in `X-Asyncdb-Term` on every write the leader orders, and a copy
  refuses anything older than the newest term it has applied (`stale_leader`, 409). A partition
  nothing leads yet answers `no_leader` (503) to a write and serves reads as normal. **The term is
  what tells the two write hops apart**: a write *to* the leader carries none, a write *from* it
  carries the term.
- `DEBUG(...)` from `src/log.h` compiles to nothing unless the `LOG` define is `1`; `recipe.json` sets
  `"LOG": "echo 1"` (the define values are shell commands that Cheesemake evaluates).

### Tests

`test/` mirrors `src/`. Unit tests substitute `repository::fake_repository` (an in-memory map) for the
RocksDB implementation, `cluster::fake_cluster` for the cluster and `http::fake_client` for the
network. `server_test` is an integration test: it starts a real server on port 0 in a thread and
drives it with libcurl. `test/server/cluster_test.cpp` is the same thing twice over: two real servers
on two ports, each given a `cluster::cluster` naming the other, so forwarding, table fan-out and
merged scans are exercised over real sockets. Both have to stop the servers they start.

**The store is one directory, named by `ASYNCDB_DATA`.** `server::data_directory()` reads it and
defaults to `/var/lib/asyncdb`, which the image mounts a volume over — a named one per node in
`docker-compose.yml`, a bind of the host's own in `cloudformation.json` — so an instance that is
started again opens what the one before it wrote. The repository opens the directory **as it stands**
rather than something random underneath it.

**Gotcha: RocksDB locks the directory it opens**, so two servers in one process are two directories —
which is why the server constructors take one and `cluster_test` gives its two `/tmp/asyncdb/first`
and `/tmp/asyncdb/second`. Tests pass `/tmp/asyncdb` rather than take the default, and
`remove_all("/tmp/asyncdb/")` in `SetUp` — and because the directory is opened in the repository's
constructor, `repository_test` holds its repositories in a `unique_ptr` so that the emptying happens
first.

A volume survives a container, and **an instance being replaced is still an empty database**: the
root volume goes with the instance, and a node that comes back holding nothing answers
`table_not_found` for the keys it owns until the tables are declared again.

## UI architecture

Built on [@datumjs/datum](https://www.npmjs.com/package/@datumjs/datum), not a mainstream framework:

- A component is a plain class. Public fields are bound to `data-bind="fieldName"` attributes in the
  HTML; `Binding` declares behaviour (`click`, `text`, `visible`, `update`) and `Update` a
  DOM-mutation-only binding. An array field repeats its template element per item.
- HTML fragments are imported with `?url` and injected at runtime by `fetchPage(element, html)` inside
  `onBind(element)` — the markup is fetched, not bundled, so `ui/html/**` ships as separate files.
- `App` wires a `NavPiece` (routes `tables` / `newTable`) and a side bar that `Tables` opens with a
  `TableDetail`. `fetchPage` and `DatabaseClient` are injected, so tests pass `() => {}` and
  `FakeDatabaseClient` and then reach into `app.currentPage.datumPiecesCurrentPage` to assert.
- `Tables.#buildGraph` / `#buildRow` do the layered DAG layout: dependency-free tables form row 0, then
  each row takes tables whose dependencies are all already placed (max 6 per row), sorted to sit near
  their dependencies. Positions feed the SVG in `ui/html/table/tables.html`.
- `ui/vite.config.js` aliases `~` to the `ui` root and carries a `rename-datum` plugin working around
  `@datumjs/pieces` still importing the pre-rename `"Datum"` package — needed in both Vite and Vitest.

## Release

Pushing to `master` builds the Docker image and, **only if the tag in the `version` file does not already
exist in ECR**, pushes it, writes that tag to the SSM parameter `/asyncdb/version` and git-tags the commit.
It then `make create-stack`s the CloudFormation stack, waits for `/health` to name six nodes, runs the
Postman collection against the stack's `Url` output with `newman`, then the Playwright journeys and
then `perf/write.sh` and `perf/read.sh` against that same address, and `make delete-stack`s it again —
whether they passed or not, so a failing assertion, journey or load run fails the build and still
leaves nothing running. The teardown deletes only a stack that same run created, so a stack standing
by hand makes `create-stack` fail and is then left alone (`ClusterALB` is a fixed name, so there can
only be one).
Bump `version` to cut a release; leaving it unchanged makes CI a no-op publish. AWS infrastructure
lives in `cloudformation.json`, driven by the `Makefile` (`make create-stack` / `update-stack` /
`delete-stack`), and is documented in `doc/deployment/`.

`version` is the only place the tag is written by hand. The template's `Version` parameter is an
`AWS::SSM::Parameter::Value<String>` defaulting to `/asyncdb/version`, so `make update-stack` resolves
the tag at deploy time from what CI actually published rather than from the working tree — which is why
the Makefile passes no parameter, and why passing one means passing the *parameter name* and never the
tag. A `LaunchTemplate` change does not recycle running instances, so a release reaches an instance only
when that instance is replaced. **The parameter has to exist before the first deploy**: CloudFormation
cannot resolve it otherwise, and an instance that cannot pull its tag has no container at all, fails the
ALB health check on `/asyncdb/health`, and is replaced by another that cannot pull either — the load
balancer answers 502 throughout.

The database tier is **six** instances, `DesiredCapacity: 6` across three subnets, which an auto
scaling group balances into two per availability zone — three copies of the keyspace (one per zone,
because `ASYNCDB_ZONE` is the instance's real AZ), each split in half between that zone's two nodes.
Capacity is worth moving three at a time so that no zone holds a larger share than the others.

The etcd tier is three instances at addresses fixed in the template's `Etcd` mapping, not a discovery
service: `ASYNCDB_ETCD` is `Fn::FindInMap` of that same mapping, which is what joins the database tier
into a cluster.
