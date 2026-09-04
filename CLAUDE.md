# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

A C++20 HTTP/JSON database server for asynchronous data processing (Boost.Beast + RocksDB), plus a
vanilla-JS single-page UI that draws the tables and their dependencies as a DAG. Several instances
partition a keyspace between them, finding each other through etcd — see
[Clustering](#clustering).

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
- The `verify` phase memchecks under valgrind, which is why `cmk verify` takes minutes rather than seconds.
  Cheesemake's own `valgrind.chevre` runs `build/bin/asyncdb`, and that serves until it is signalled, so the
  root `valgrind.chevre` overrides it and runs `build/test/test_main` instead. The suite is clean of definite
  and possible leaks, but the plugin only reports: `--error-exitcode=1` is deliberately not set, because the
  release image builds on musl, where a leak of glibc's or RocksDB's own would fail the build untested.
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
`clusterSize` variable is what makes folder 8 assert a three-node cluster or a lone instance.

### Browser tests (`automation/`)

Playwright, end to end over the UI in Chromium, and no part of `cmk`:

```bash
cd automation && npm install && npx playwright install chromium   # once
npm test
```

The config starts `vite` in `ui/` on port 4173 itself, and `FakeDatabase` fulfils the
`/asyncdb/table*` requests with `page.route`, so no server has to be up — the real `DatabaseClient`
is still what sends them. Specs match `*Test.js`, as in `ui/test`.

### Load tests (`perf/`)

`perf/read.sh` and `perf/write.sh` share `perf/harness.sh`, which forks `THREADS` curl workers over
persistent connections and reports latency percentiles. Everything is an environment variable:
`THREADS`, `REQUESTS`, `BASE` (default `http://localhost:8080/asyncdb`), plus `URL` for reads and
`TABLE` and `VALUE_BYTES` for writes. They drive a server that is already running and are
no part of `cmk`.

### Running the whole thing

`docker-compose up`, then the UI is on `localhost:8080`. The image runs nginx on port 80 serving
`ui/dist` and reverse-proxying `/asyncdb/*` to the `asyncdb` binary on `localhost:8080`
(`server/server.conf`); that is why `DatabaseClient` uses relative `asyncdb/...` URLs. The compose
file brings up **three** instances (`localhost:8080`, `8081`, `8082`) and the etcd they partition
their keyspace through, so any of them answers for every key.

### Clustering

`ASYNCDB_ETCD` (where etcd answers — one base URL, or every member of the etcd cluster separated by
commas, tried in turn and sticky on whichever answered) and `ASYNCDB_NODE` (this node as the others
reach it, the API port and not the nginx in front of it). **Set neither and nothing changes**: no
thread is started, nothing is registered, and the instance owns the whole keyspace, which is what
every test that is not `cluster_test` runs as. Set both and the instance joins.

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

- **`server::server`** owns the `io_context`, the acceptor and a thread pool sized to
  `hardware_concurrency()`. It also owns the single `rocksdb_repository` and `router`, which are shared
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
  keeps the last request's body, or a HEAD's "no body", out of the next one.
- **`cluster::cluster`** is the second pure-virtual seam the router routes against, over "who owns
  this key" and "ask that node". `cluster::standalone` owns everything and is what a router built
  without a cluster gets; `cluster::etcd_cluster` registers `/asyncdb/node/{address}` in etcd on a
  lease, renews it on a thread of its own, and reads the membership back. `cluster::owner_of` is
  rendezvous hashing over that membership, and `cluster::forward` is how a request travels.
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
- **Partitioning is by key alone, never by table**, so the same key of two tables is on one node and
  a record and the records derived from it are one hop. A record request goes to the owner; a table
  create or delete goes to *every* node, because a record can only be written where its table is; a
  scan is asked of every node and merged back into key order. A forwarded request carries
  `X-Asyncdb-Forwarded` and is served where it lands, which is what stops two nodes bouncing it.
  `doc/database/cluster.md` is the spec, including what partitioning deliberately does not do (no
  replication, no rebalancing).
- `DEBUG(...)` from `src/log.h` compiles to nothing unless the `LOG` define is `1`; `recipe.json` sets
  `"LOG": "echo 1"` (the define values are shell commands that Cheesemake evaluates).

### Tests

`test/` mirrors `src/`. Unit tests substitute `repository::fake_repository` (an in-memory map) for the
RocksDB implementation, `cluster::fake_cluster` for the cluster and `http::fake_client` for the
network. `server_test` is an integration test: it starts a real server on port 0 in a thread and
drives it with libcurl. `test/server/cluster_test.cpp` is the same thing twice over: two real servers
on two ports, each given a `cluster::cluster` naming the other, so forwarding, table fan-out and
merged scans are exercised over real sockets. Both have to stop the servers they start.

**Gotcha:** the RocksDB directory is hardcoded to `/tmp/asyncdb` in `server::server`'s constructor, and
each process opens a *randomly named* subdirectory of it, so data does not survive a restart and stale
directories accumulate. Tests `remove_all("/tmp/asyncdb/")` in `SetUp` — and because the directory is
opened in the repository's constructor, `repository_test` holds its repositories in a `unique_ptr` so
that the emptying happens first.

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

The etcd tier is three instances at addresses fixed in the template's `Etcd` mapping, not a discovery
service: `ASYNCDB_ETCD` is `Fn::FindInMap` of that same mapping, which is what joins the database tier
into a cluster.
