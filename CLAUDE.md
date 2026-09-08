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

**`VALUE_BYTES` and `REQUESTS` trade against each other.** The script default is a kilobyte against
`REQUESTS=5000` on sixteen threads; `build.yaml` loads the deployed stack with two megabyte values
and thirty two requests a thread instead — values the proxy and the parser in front of the store
refused outright until both were raised to the documented 16 MiB, and half a gigabyte in each
direction either way. The write's curl config clears `Expect:` for the same reason `http::client`
does: **only the nginx in front of the database answers a 100 Continue**, so a body over a kilobyte
otherwise spends a second of curl's own timeout per request wherever the proxy is not in the path —
1150 ms a write against the binary alone, against 190 ms through nginx.

**They are tests as well as measurements.** A request the server answers with anything but a 2xx —
including the `000` of a transfer that never answered, and the 2xx of one whose body the connection
cut short — makes the run exit non-zero, which is what lets `build.yaml` run them last against the
deployed stack and fail the build on them. A status alone would miss the second: it is the one the
server sent before the transfer broke, so `%{exitcode}` is counted beside it. Latency is
reported and never asserted on: nothing here is a threshold.

### Chaos tests (`chaos/`)

The failure modes in `doc/runbook`, injected into the **deployed AWS stack** with the AWS CLI and
asserted on from outside. No part of `cmk`, and the only suite here that breaks the thing it is
testing:

```bash
make create-stack            # or the stack a build stood up
make create-chaos-stack      # chaos/chaos.yaml: the permission to inject a fault
chaos/validate.sh            # every experiment's preflight, nothing applied — seconds
chaos/run.sh                 # all ten experiments, in order
make delete-chaos-stack
```

`chaos/harness.sh` is sourced by each experiment the way `perf/harness.sh` is, and owns the same
four things every one of them needs: the stack, the fault, a probe recording what a client saw
while it ran, and the verdict. Everything is an environment variable — `CHAOS_EXPERIMENTS`,
`CHAOS_SETTLE`, `CHAOS_RECOVERY`, `CHAOS_CONVERGE` — and a failed assertion is a non-zero exit,
which is what lets `build.yaml` run it after the load tests and fail the build on it.

**An experiment declares three functions and the harness owns when they run**: `inject` applies
the fault, `heal` takes it away, and `preflight` asks whether `inject` would work while applying
nothing. `fault_start` and `fault_stop` are the two calls an experiment makes, so the rest of the
script is the assertions and nothing else. `heal` is called by the exit trap as well, which is why
every one of them is written to be safe run twice, or against a fault that never landed.

**A duration is a ceiling and nothing else.** A fault lasts until `fault_stop` takes it away, which
is as soon as that experiment's assertions are done; the seconds an experiment names are what its
script sleeps for if nothing ever comes back to remove it. That is what lets them stay generous:
**a fault that expires mid-assertion is a false failure and not a weaker test**, because
`scan-loses-a-node` asserts that a scan *fails* while a node is deaf. None of it is taken on trust:
the recovery assertion every experiment runs next is the check that the fault went, **and it has to
be one the fault would fail**. A rule left standing changes no membership, so `scan-loses-a-node`
waits on a write rather than on `/health`: a write needs every copy, and a node no peer can reach
fails one.

- **The suite refuses to start** against a cluster that is not already six nodes in three zones
  with nothing stalled, and stops early if an experiment's damage did not heal — everything
  after that would be measuring the previous fault.
- **Order matters.** The three that need nothing of the instances are first; the three that
  resize the tier come after every fault that only breaks it, because they are the only ones that
  change what the deployment *is*; `etcd-quorum-lost` is last, because it is the only one that
  leaves the cluster having been wrong about itself, and the pipeline deletes the stack next.
- **Four of the ten inject through `ssm:SendCommand`** and the `AWS-RunShellScript` document,
  which needs the private subnets' [route out](#release): `node-latency` installs `tc` from the
  distribution repositories, so what it depends on is an instance being able to install a package
  while it is under test. All four send the script `fault_script` builds — write the removal down,
  arm a detached timer, install, sleep, remove — and none of them waits that sleep out: `heal`
  takes the fault away over a second Run Command, and the timer is for the run that died holding
  it. `scan-loses-a-node` and `etcd-unreachable` install their rule in the **`DOCKER-USER`** chain,
  because a container behind a published port is reached through `FORWARD` and sends through it
  too, so **a rule in `INPUT` or `OUTPUT` blocks nothing here**. A deaf node is still asked over
  Run Command, because a request the host makes to a published port never crosses `FORWARD` —
  which is how the rule is taken out again, and how `scan-loses-a-node` asks a scan of a node that
  hears rather than of whichever one the load balancer picked. The other three (`ec2:StopInstances`
  twice, and a network acl on one zone's subnet) need nothing of the instances, which is why they
  are first. `chaos/README.md` is the page.
- **Three of the ten inject with a stack update**, because the shape of the database tier is two
  parameters of `cloudformation.yaml` and nothing else: `Zones` is how many copies of the keyspace
  there are — a zone holds exactly one — and `Nodes` is how many ways a zone splits the copy it
  holds. `zone-retired` takes the replication factor from three to two and back, `nodes-added`
  takes the tier to nine instances and `nodes-removed` to three, and `heal` is the update back, so
  a run that dies inside one leaves the stack the shape it found it. **`zone-retired` then stops
  the instances the group is no longer allowed to keep**, because a group given one subnet fewer
  rebalances out of the one it lost in its own time — a quarter of an hour of the scheduler's
  pacing, which says nothing about this system. Stopping them is what `doc/deployment/database.md`
  documents as the procedure: the health check is `EC2`, so the group terminates them and launches
  the replacements in the subnets it still spans, and both go at once, so the zones that stay
  redraw their split while the replacements are still booting. They carry
  `--use-previous-template`: what is under test is the stack the pipeline stood up. Three is the
  ceiling for `Zones` and two the floor, so **the increase in replication is asserted on the way
  back** rather than as a fault of its own — and that half is what `zone-lost` cannot test, because
  a zone cut off comes back with its copy and a zone retired comes back with instances that have
  never held anything.
- **The three resizes are the test of `reconcile`.** Each asserts two invariants after every
  transition: **every zone holds the same keys** (a zone holds a copy of the whole keyspace, so two
  zones naming different keys is a copy that is short) and **no key is held by two nodes of one
  zone** (a zone's nodes split the copy it holds). The first is the fetch half of a reconcile pass,
  the second the clear down half, so which one fails says which half of the mechanism broke.
- **Neither invariant can be seen through the load balancer**, which answers a read from whichever
  copy has the key and so says a record exists somewhere and never where. `holdings` in
  `chaos/harness.sh` asks each node what is in its own store, over Run Command, with a scan carrying
  `X-Asyncdb-Forwarded` — served where it lands, so it is that node's own share and not its zone's
  merged answer, which is the request a rebuild makes of each node of a zone. A node that cannot be
  asked is a failed assertion and not an empty store.
- **What a terminated instance took with it is measured and never asserted.** A key whose owner in
  every zone was terminated by the same update went with them, and nothing in the cluster puts that
  back — no rebuild of a copy, no backup. Every resize prints how many seeded keys are still held
  somewhere. What is asserted about a failed read is its shape: a 2xx or a 404, never a 5xx and
  never a request that did not answer.
- **`disk-fills` tests the proxy as much as the store.** nginx spools a request body over 8 KiB
  to a temporary file, so a full volume answers `500 unavailable` out of `server/50x.json` before
  the database is asked at all — which is why the experiment writes in two sizes, a megabyte the
  proxy refuses and a kilobyte that reaches RocksDB. What the store does with the kilobytes is
  reported and not asserted: the write ahead log is preallocated, so a node whose disk filled a
  minute ago still has tens of megabytes reserved to write into. What it asserts instead is that
  every write answered `2xx` while the disk was full is still there afterwards.
- `node-stops` is the only test anywhere of the rebuild in `doc/runbook/rebuild.md` as a
  *replacement* runs it; `nodes-added` and `zone-retired` reach the same mechanism from a tier
  that grew, and `zone-retired` is the only one that has a whole zone's copy built.
- **`chaos.yaml` is the permission to break things.** `ChaosPolicy` attaches stopping and
  starting an instance tagged `asyncdb` or `etcd`, writing a network acl, sending a Run Command,
  updating the stack under test and suspending the etcd group's `ReplaceUnhealthy` to the IAM
  groups in the `Operators` parameter (default `builders`, which holds the pipeline's identity).
  Nothing else in the account
  grants the destructive half of that, so outside a chaos run nobody here can stop an instance of
  either tier or run a shell command on one. `make update-chaos-stack` applies a change to a chaos
  stack that is already standing.
- **Run `chaos/validate.sh` after touching an experiment.** It runs every `preflight` and no
  fault at all: the targets are resolved, EC2's own `--dry-run` answers the calls that offer one,
  Systems Manager is asked whether the agent answers on the instances a fault would go through,
  and a resize creates the change set its update would apply, reads it and deletes it. That is
  the whole of "would this run?" for a handful of API calls and nothing applied — a chaos stack
  that is not standing, an instance whose agent never registered, a group whose logical id moved
  and a stack whose template predates the two resize parameters are caught in seconds rather than
  by a full run.

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
lead it. No zone named anywhere is one zone holding all the nodes, and so one copy of the keyspace.
`docker-compose.yml` runs two zones (nodes 1 and 2 in `one`, node 3 in `two`) so the compose
cluster both partitions and replicates; `cloudformation.yaml` reads
the real AZ out of IMDS, which is three zones of one node each.

## Comments and documentation

- **Comment only what the code cannot say**, and keep it short. Most code needs none. An invariant, a
  constraint imposed from outside the file, or a reason the obvious thing is wrong is worth a line or
  two; a paragraph almost never is, and a comment restating the code is worse than no comment.
- **Write about the design as it stands, never about how it got that way.** No changelogs, no "used
  to", no "this replaced X", no account of the bug that prompted the current shape. State the rule
  and the reason it holds now. `git log` is where the past is kept.
- Both apply everywhere: `CLAUDE.md`, `doc/`, every `README.md`, and comments in C++, JavaScript,
  shell, YAML and config alike.

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

Beside that, two passes that move records between nodes rather than serving anybody:
`rebuild::rebuild` fills an empty store before the node joins, and `reconcile::reconcile` moves the
records whose owner moved, on a thread of its own, whenever the membership changes.

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
- **The session reads through a parser of its own, and that is what makes the documented limits
  reachable.** Beast's defaults are a 1 MiB body and an 8 KiB header, against an API that documents a
  16 MiB value and a 4 KiB key — and a key travels percent encoded, so three bytes to the byte and
  12 KiB of request line at worst. Neither default is an error the server answers: **the read itself
  ends in one and the connection closes with nothing written on it**, which is a dead socket rather
  than `value_too_large` and kills a forwarded copy of a large value between two nodes. `read()`
  therefore sets `body_limit(record::max_value_size + 1)` — one byte over, so the router is what
  refuses an oversized value — and `header_limit(3 * record::max_key_size + 8 KiB)`.
  **`server/server.conf` carries the same two limits for the nginx in front of it** (`client_max_body_size`,
  `large_client_header_buffers`), and a body over its limit is answered `413 value_too_large` from
  `server/413.json` rather than nginx's own HTML. Change one of the four and change its pair.
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
  handle map is guarded by a `shared_mutex`. Every write goes through `written` rather than `check`,
  which `Resume()`s a store RocksDB stopped for a background error before reporting the failure:
  a write that failed for want of space is otherwise sticky, and a disk with room on it again would
  be a node refusing every write to half the keyspace until somebody restarted it.

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
  **The accepted characters are `[A-Za-z0-9_ -]`** — either case, digits, space, underscore, hyphen.
- `record::parse_record` enforces the limits (4 KiB of key, 16 MiB of value) and that a key is valid
  UTF-8. A value is never looked at — every string is a value, and the empty one is told from a missing
  key by the status code, which is why `read_record` returns a `std::optional`.
- `scan::range` is the parsed query of a scan or a range delete, and a cursor is base64 of
  `{ "k": last key, "s": instance }`; the instance is what makes a cursor this instance did not issue
  refusable.
- **A page is bounded in bytes as well as in records** — `scan::max_page_bytes`, 8 MiB of keys and
  values. `limit` caps the count and says nothing about the size, so a thousand of the largest legal
  records is a 16 GiB response built in memory on the node answering: the instance dies, is replaced,
  and comes back empty. The walk in `rocksdb_repository::scan_records` stops on the budget and sets
  `has_more`, and `trim_to_budget` in the router applies it again to the merge, because each node
  answered within it but a zone of two nodes is two pages of it. **A record larger than the whole
  budget is still returned, alone**, or a scan could never get past that key. `fake_repository`
  walks the same way, so a unit test sees the page a client really gets.
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
  repair, no replication log, and a write that needs both a leader and every copy).
- **Records move when ownership moves, and only then.** A membership change redraws the split
  inside a zone without moving a record, so `reconcile::reconcile` does: it **fetches** what this
  node now owns and holds nothing for, from every other node, and **clears down** what it no longer
  owns. `server::server` runs it on a thread of its own, one tick *after* the membership it saw
  changed — a membership is a moment, and a store that matches the one it was left with is never
  walked, which is why a test naming its own static cluster never runs a pass at all.
  **What makes the delete safe is that it asks first**: a copy is given up only when the node that
  owns that key *in this node's own zone* answers a HEAD saying it holds it. Not any copy — the
  record here is this zone's copy, so deleting it because another zone has one is a zone left
  holding nothing — and a node acting on a view that is a moment out of date is told no, keeps the
  record, and asks again next pass. The count of those is `deferred`, and a pass with any is not
  settled, so it runs again. **Neither half is safe alone**: clearing down without fetching is a
  shrink that loses records rather than staling them; fetching without clearing down is a store
  that only grows and a stale value waiting for the membership to swing back. Both are best effort
  and caught like the rebuild, because a store that refuses a write must not take the process down
  from a thread of its own. `doc/runbook/rebuild.md` is the page.
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
`docker-compose.yml`, a bind of the host's own in `cloudformation.yaml` — so an instance that is
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

Pushing to `master` builds the Docker image and uploads it as a workflow artifact. That first job,
`build`, has no condition on any step and asks AWS nothing: it builds, reads `version`, answers the
gate and hands the image on.

**Everything that publishes or costs anything is downstream of the second job, `publish`, gated on
the tag in the `version` file not having passed the suite already, so it runs once per version
rather than once per push.** `publish` loads the artifact and pushes it to ECR, creating the
repository `asyncdb` if the account has none, writes that tag to the SSM parameter
`/asyncdb/version`, mirrors the etcd tag `etcd-version` names into ECR if it is not there already
and writes `/asyncdb/etcd`, and `make create-chaos-stack`s the permission to inject a fault, once
for every share below it.

**`verify` is then a matrix of four, one stack each, `fail-fast: false`.** Each share
`make create-stack`s `asyncdb-{one,two,three,four}` — `STACK` and `CHAOS_STACK` come from the matrix —
waits for `/health` to name six nodes, runs `chaos/validate.sh` and `chaos/run.sh` over the
experiments the matrix names it in `CHAOS_EXPERIMENTS`, and `make delete-stack`s it again whether
they passed or not. **The share carrying `matrix.suites` also runs the Postman collection, the
Playwright journeys and `perf/write.sh` / `perf/read.sh` first**, before anything has broken its
stack. The shares are balanced by measured time — about ten minutes of experiments each out of the
forty the ten of them take, with the suites counting as four beside them — so all four land within a
minute or two of twenty-three. `doc/pipeline/index.md` is the page. **An experiment nobody names in
the matrix is an experiment nobody runs**: there is no default list in the workflow.

`release` then pushes the git tag and `cleanup` deletes the chaos permissions. A share's teardown
deletes only a stack that share created, so a stack standing under one of those three names makes
`create-stack` fail and is then left alone — while a stack standing by hand as `asyncdb` collides
with nothing and only costs quota. **Four stacks at once is four VPCs, four load balancers and
thirty-six `t3.micro`**, against a default of five VPCs to a region — so the account has room for
the run and a default VPC and nothing else, which is what sizes it. **Four is where more stacks
stop paying**: a share pays about eight minutes to stand its cluster up and tear it down against
about ten of work, so a fifth stack takes two or three minutes off a twenty-three minute run and
costs a whole VPC, a load balancer and nine instances for them.

**There is one gate, and it is the `{version}` git tag.** It is the `if:` on `publish` and
**nowhere else in the workflow** — no step repeats it, and every job that costs anything is
downstream of `publish` by `needs:`, so a skipped `publish` skips them all. It is
`git ls-remote --exit-code --tags origin refs/tags/$VERSION` finding nothing, carried across the job
boundary as an output. The tag is pushed by `release`, which needs every share of `verify` and
carries no `if:` of its own — the same mechanism one level up: a job with no condition runs only
when every job it needs succeeded, as a step with no condition runs only when every step before it
did. The `if:`s that do appear on steps are about something else — `matrix.suites` picks the share
that runs the API, browser and load suites, and `always()` marks the teardowns — and none of them
So a version that fails is published and retried on every push until it passes, and a version that
has passed is neither republished nor stood up again — which is what keeps thirty-six instances
and four load balancers off a push that only touched a comment. **A version tag means
passed, and never merely published**; the workflow asks git alone, and neither ECR nor
`/asyncdb/version` is a question it puts.

**The image tag in ECR is therefore overwritten**, for as long as the version has not passed — the
repository is created mutable, which is the default. It has to be: every stack pulls the image it
tests out of ECR, so the push is what `publish` does before a share stands anything up, and every
commit carrying a red version has to be the one the suite then runs against. The window in which a version's bytes
move is exactly the window before it passes, so **do not pull a version that has no git tag**.

**Nothing is held outside the repository to make that work**: `git tag -l` is the list of versions
that have been through the suite, and `git push --delete origin {version}` is how one is made to go
through it again without bumping it — which rebuilds and republishes it, rather than re-testing what
is in ECR. The gate asks the remote rather than the working tree, because the checkout is one commit
deep and fetches no tags; `--exit-code` is 0 for found and 2 for not, and a remote that cannot be
reached at all is 128, which reads as not verified and both publishes and deploys. **The gate fails
towards spending money, never towards skipping a suite that should have run.**
Bump `version` to cut a release; leaving it unchanged makes CI a no-op publish that deploys nothing.
AWS infrastructure lives in `cloudformation.yaml`, driven by the `Makefile` (`make create-stack` /
`update-stack` / `delete-stack`), and is documented in `doc/deployment/`.

`version` is the only place the asyncdb tag is written by hand (`etcd-version` is the same thing for
the mirrored etcd tag — see [Machine images](#machine-images)). The template's `Version` parameter is an
`AWS::SSM::Parameter::Value<String>` defaulting to `/asyncdb/version`, so `make update-stack` resolves
the tag at deploy time from what CI actually published rather than from the working tree — which is why
the Makefile passes no parameter, and why passing one means passing the *parameter name* and never the
tag. A `LaunchTemplate` change does not recycle running instances, so a release reaches an instance only
when that instance is replaced. **The parameter has to exist before the first deploy**: CloudFormation
cannot resolve it otherwise, and an instance that cannot pull its tag has no container at all and fails
the ALB health check on `/asyncdb/health`. The group's health check is `EC2`, so nothing replaces it
either: it sits there running nothing, and the load balancer answers 502 throughout.

**Both tiers are in private subnets**, and where a NAT gateway would be there is an
`EgressOnlyInternetGateway`: the private subnets are dual stack (`Ipv6CidrBlock` gives the VPC an
Amazon `/56`, `Fn::Cidr` cuts a `/64` for each of them, `AssignIpv6AddressOnCreation` is on), and
`PrivateRoute` sends `::/0` to that gateway. **No instance has a public IPv4 address, there is no
`0.0.0.0/0` anywhere in `PrivateRouteTable`, and nothing outside the VPC can open a connection to
an instance** — an egress-only gateway is outbound only and stateful — but an instance can open one
outwards over IPv6. There is no SSH and no key pair (Session Manager is the way onto an instance),
and the three public subnets hold the ALB alone.

**Everything an instance calls therefore has to be named in its dual-stack form**, because an AWS
endpoint is IPv4-only otherwise: both user data scripts export `AWS_USE_DUALSTACK_ENDPOINT=true`
for the CLI (`ecr.…api.aws`, `ec2.…api.aws`), pull from `332187735950.dkr-ecr.eu-west-2.on.aws`
rather than `dkr.ecr.eu-west-2.amazonaws.com`, and write `UseDualStackEndpoint` into
`/etc/amazon/ssm/amazon-ssm-agent.json` before restarting the agent, which is what keeps Session
Manager and the Run Commands `chaos/` injects with working. Get one of those wrong and the instance boots
with no container and is replaced by another that does the same. `doc/deployment/network.md` is
the page.

The database tier is **six** instances, `DesiredCapacity: 6` across three subnets, which an auto
scaling group balances into two per availability zone — three copies of the keyspace (one per zone,
because `ASYNCDB_ZONE` is the instance's real AZ), each split in half between that zone's two nodes.
Capacity is worth moving three at a time so that no zone holds a larger share than the others.

The etcd tier is a second auto scaling group of three, one per AZ, with no fixed addresses anywhere:
every instance of both tiers calls `ec2:DescribeInstances` at boot, filtered to `Name=etcd` in this
VPC. An etcd node builds `--initial-cluster` from the answer and either bootstraps, starts as a member
somebody else's bootstrap already named, or **prunes the members no instance answers for and
`member add`s itself** — which is what makes a replaced instance rejoin, and it starts from what the
add printed. A node that finds a cluster it cannot join (no quorum) exits rather than bootstrap a
second one. `ASYNCDB_ETCD` is the same query without the join, read **once at boot**, so replacing
every etcd instance without rolling the database tier strands it. `doc/deployment/etcd.md` is the page.

### Machine images

**There are none, deliberately.** Both tiers launch from `BaseAmi`, the public parameter
`/aws/service/ecs/optimized-ami/amazon-linux-2023/recommended/image_id`, resolved by
CloudFormation at deploy time — the ECS-optimised Amazon Linux 2023, taken for the Docker daemon
already on it and **not** for ECS: there is no cluster, no task definition and no agent doing
anything. Each tier's user data does its own host preparation, which is `systemctl enable --now
docker`, one `mkdir` and the dual-stack lines above, and then pulls its container from ECR.

**The etcd container is mirrored into this account's ECR rather than baked**, which keeps quay.io
off the boot path — a private subnet has no route to it — and puts the tag on the pipeline that
already runs. The price of taking a base image AWS republishes patched is that two instances of one
auto scaling group launched a fortnight apart can be two different operating systems; that is
accepted, and pinning it is one parameter override away.

**The etcd tag is written by hand in one place, `etcd-version`.** The build reads it, mirrors
`quay.io/coreos/etcd:$ETCD_VERSION` into ECR if it is not there already, and writes
`/asyncdb/etcd`, which the template's `EtcdVersion` parameter resolves — exactly the arrangement
`version` and `/asyncdb/version` have for the asyncdb image. It sits in `publish` beside
that push, so it runs when a stack is about to pull the tag and not otherwise: **bumping
`etcd-version` alone mirrors nothing**, and wants a `version` bump with it. `docker-compose.yml`
names the same version against quay.io directly, because a laptop has an internet connection.
`doc/pipeline/index.md` is the page.
