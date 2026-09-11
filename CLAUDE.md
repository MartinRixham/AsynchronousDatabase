# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

A C++20 HTTP/JSON database server for asynchronous data processing (Boost.Beast + RocksDB), plus a
vanilla-JS single-page UI that draws the tables and their dependencies as a DAG. Several instances
partition a keyspace between them and keep one copy of it in each availability zone, finding each
other through etcd — see [Clustering](#clustering).

## Working here

**A change already in the working tree is deliberate.** Take an edit, a deletion or a revert as
intended and build on it: do not flag it, restore it, or ask whether it was meant. A comment or a
block that is gone was removed on purpose, and a change sitting beside the one that was asked for
is still a change that was chosen. Raise one only where it breaks what you were asked to check —
a failing build, a red test, an invariant that no longer holds — and then report what broke rather
than what to put back.

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
build/test/test_main --gtest_filter='table_test.fail_to_deserialise_table_with_no_name'
```

Notes:
- `-Wall -Werror` — any warning fails the build. `cppcheck --enable=style` runs in the `validate` phase.
- The `verify` phase memchecks under valgrind, which is why `cmk verify` takes minutes rather than seconds.
  Cheesemake's own `valgrind.chevre` runs `build/bin/asyncdb`, and that serves until it is signalled, so the
  root `valgrind.chevre` overrides it and runs `build/test/test_main` instead, keeping the report in
  `build/test/test_main.valgrind`. **A definite or possible leak fails the build**, and the suite is clean of
  both. It is the plugin's own grep of the leak summary that fails it, and not `--error-exitcode=1`: that
  would fail on every error valgrind reports, still reachable included, and the release image builds on musl,
  where a block libstdc++ or RocksDB still holds at exit would fail the build untested. `valgrind.supp` is
  the exception the plugin is given on the command line, and it names one block and no more: the thread_local
  RocksDB registers on a background thread of the default `Env`, which it never destroys, so the thread is
  still running when the process ends and nothing here can free what it holds. A leak that is ours belongs in
  the summary, where the build fails on it.
- **A test that starts a server has to stop it.** `serve()` returns only when the acceptor is closed, and an
  always-pending accept holds a `shared_ptr` to the server, so a detached serving thread leaks the server,
  its thread pool and its RocksDB. `server_test` closes and joins in `TearDown`.
- `compile_flags.txt` is for clangd only; the real flags come from `recipe.json`.
- Formatting is enforced by `.clang-format` (tabs, Allman braces, 120 columns, `SortIncludes: false`).
- Naming is `snake_case` throughout, including class names, and each layer lives in its own namespace
  matching its directory.
- **One class to a file**, and the file is named after it — `curl_client` in `src/http/curl_client.h`,
  its definitions in `curl_client.cpp`. A helper only one `.cpp` reaches is still a file of its own.
  A class whose name would only repeat its namespace carries the namespace in the file name
  instead — `http::client` is `http/http_client.h`, `etcd::client` is `etcd/etcd_client.h`, and
  `http::fake_client` is `test/http/fake_http_client.h`.
  A struct that is only data sits beside whatever it belongs to (`http::request` in `http_client.h`,
  `cluster::placement` in `cluster.h`), and a test fixture belongs in the test file it is the fixture
  for; nothing else shares.
- **What a call has to say is its return type, never an out parameter.** A compound answer is a
  struct — `scan::page` is the records and whether there are more, `cluster::placement` is whether
  this node holds a copy and which other nodes do — and an answer that may be missing is a
  `std::optional`, as `read_record` and `base64::decode` are. A pointer parameter the callee writes
  through puts half the answer in the return and half in the arguments, and leaves the caller holding
  an object that means nothing until the call has been made. What is not an out parameter is a sink
  the caller already owns: libcurl writes a body into the `http::response` it carries, and `merge`
  sorts the records it is given.
- **A field of an abstract type is handed in, never chosen inside the class.** A class holding a
  `cluster::cluster &`, an `http::client &` or a `repository::repository &` takes it as a parameter of
  its one public constructor, and every caller — `main.cpp` and every test — supplies one. No second
  constructor taking a raw pointer for the first to delegate to, and no concrete member to fall back
  on when that pointer is `NULL`: a fallback makes the class own the implementation the seam exists
  to keep out of it, and leaves the object carrying two candidates for one field. Where the class
  needs a call the seam does not carry, the seam grows it — `start`, `discover` and `stop` are on
  `cluster::cluster`, so `server::server` joins and leaves the cluster it routes through rather than
  one of its own.

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
  with nothing stalled and every node holding what it owns, and stops early if an experiment's
  damage did not heal — everything after that would be measuring the previous fault. `every_node_whole`
  in `chaos/harness.sh` is the last of those, and it asks **each node** over Run Command rather
  than sampling `/health` through the load balancer: `incomplete` is a node's own state, and a
  node that came back from a rebuild short of its share is in the membership and answering, so
  the shape cannot show it. A node that cannot be asked fails it. It runs at the start and after
  every experiment, which is what makes `node-stops` and the three resizes a test of the rebuild
  and not only of the records that moved.
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
Set the first two and the instance joins. **`ASYNCDB_UNLED_WRITES` is the fourth**, and the only one
of them that is not about joining: false is a node taking a write only where a leader claimed in etcd
ordered it — a table create or delete included, since the tables are led as well — so a membership
too small to claim anything, no etcd reached or this node alone registered in it, answers
`no_leader` rather than writing what nobody ordered. It defaults to true, which is the lone instance
every test and `cmk run` serve, and the `Dockerfile` sets it false, because a container is a node of
a cluster and one on its own there has lost the others. **It is also what takes such a node out of
the load balancer**: `cluster::is_unled()` is that same membership having been too small for longer
than a lease, and `/health` reports it as `unled` and answers `503` rather than `200` — the document
unchanged, because the status is for the load balancer and the fields are for whoever is reading the
node. A lease is what makes it a state and not a moment, a membership that just fell to one being a
slow answer from etcd as often as a node that has lost the others. Nothing replaces the instance
over it, the group's health check being `EC2`; and a target group with nothing healthy left in it is
one the load balancer sends to all of them, so etcd lost altogether is a cluster that goes on
serving what it holds. The four are read in one place,
`cluster::from_environment()` in `cluster/etcd_cluster.h`, which fills a `cluster::config`: the
endpoints, this node and its zone, that flag, and beside them the tunables nothing sets from
outside — a ten second membership lease, the `/asyncdb/node/` and `/asyncdb/leader/` prefixes,
`claims_per_refresh`, and three timeouts (two seconds to connect at all, thirty to finish, and five
for etcd, which is on a shorter leash because a node that cannot reach it carries on serving what
it holds). `config::is_clustered()` — endpoints and a node name, both set — is that rule as the
code puts it, and `main.cpp` builds the config, constructs the `etcd_cluster` from it and hands
that to the server.

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
- **A test is where a claim about behaviour belongs, so a comment a test already makes goes.** A
  named test asserting what the comment says is the comment kept true by the build; the prose beside
  it is a second copy that nothing checks. Where the claim is testable and nothing tests it yet,
  write the test and then delete the comment — `test/` mirrors `src/`, and the test's name carries
  what the comment said. What is left is what no test can reach: why a constant is the size it is,
  what a lock, an atomic or a destruction order is buying, and a limit or a deployment imposed from
  outside the file. A comment that says both keeps only the half a test cannot show.
- **A comment a test contradicts is a finding, not an edit.** Report what the test showed and leave
  the comment standing: which of the two is wrong is a decision about the design, and the fix may be
  one that moves every key.

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

and, off the router, `cluster::cluster` → `http::client` → the other nodes, and
`cluster::etcd_cluster` → `etcd::client` → `http::client` → etcd.

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
  `/table/{table}/key`, `/table/{table}/key/{key}`, `/table/{table}/key/{key}/{sort}`,
  `/table/{table}/file` and `/table/{table}/split` — and returns a
  `router::response` (status, content type, and either a `boost::json::object` or the raw text of a
  value). `router/api_error.cpp`
  is the one place a documented error code is mapped to a status.
- **`http::client`** is the seam over libcurl, and `curl_client` keeps **one handle per thread**,
  reset before each request — `http::handle` is that one easy handle and `http::group` the multi
  handle and the easy handles a fan out runs on, both `thread_local` in `curl_client.cpp`, and a
  group keeps as many handles as the widest fan out that thread has run. The handle is what holds
  open connections, and the cache is sized to more nodes than a cluster has, because libcurl's own
  default is a handful — smaller than the neighbour count of anything past six nodes, which put a
  handshake back on every forward past the fifth destination a thread had used. It is a ceiling and
  not a reservation: a thread holds one connection to each node it has actually forwarded to. A fan
  out runs in the multi handle, whose cache is sized from the transfers added to it, so it is the
  single handle that needs telling — and `curl_easy_reset` is what keeps the last request's body, or
  a HEAD's "no body", out of the next one. `send_all` is the same thing for a **fan out** — the copies of a record, or
  every node of a table create — run in one `curl_multi` handle per thread, so the thread waits for
  the slowest of them rather than for the sum of them, and the multi handle holds that fan out's
  connections the way the single handle holds its own. **A fan out does not copy the bodies it is
  given**, so the requests have to outlive the call, and a fan out of one runs on the single handle
  instead. `send_all` on the cluster seam throws every answer away but a refusal, which is what a
  write to the copies of a record wants; `send_each` is the same fan out for a caller asking each
  node something *different* and reading what each of them said, which is what a walk reading a
  share in several pieces at once is.
- **`cluster::cluster`** is the second pure-virtual seam the router routes against, over "which
  nodes hold this key" and "ask that node". `cluster::replicas` answers a `cluster::placement` —
  whether this node holds a copy, and the other nodes that do, this node's own zone first.
  `cluster::zones` groups the membership for a scan: the nodes of each zone, this node's own first,
  which is why a scan asks one zone rather than every node. `cluster::holdings` is `replicas` asked of
  every partition at once, because a pass that moves records has no key to ask about: what it asks
  another node for is a share, and a share is a set of partitions. `cluster::etcd_cluster` registers
  `/asyncdb/node/{address}` in etcd on a lease with `{"node":...,"zone":...}` as its value (a bare
  address is still read, as a node in no zone), renews it on a thread of its own, and reads the
  membership back — and **a membership of fewer than two nodes is this node holding every key**,
  which is the cluster an instance told nothing runs as, so there is no second implementation of
  the seam for standing alone. The seam itself is `cluster/cluster.h`, and the rest of the
  directory is what stands behind it: `partition.h` is the hashing — `partition_of` is the 256
  partitions, `owner_of` is rendezvous hashing over a set of nodes, `owners_of` runs it once per
  zone and `zones_of` is the grouping behind `zones()`; `partition_set` is 256 bits of them and
  `encode_partitions`/`decode_partitions` are how one travels, 64 hexadecimal characters wide
  whatever is in it; `cluster::forwarder` is how a request
  travels, `forward` and `forward_all` over the `http::client` it is handed, and it is handed to
  `etcd_cluster` in turn rather than made inside it; `member.h` is `member` and `membership`,
  the whole list held as a `shared_ptr<const vector<member>>` and swapped rather than edited, so a
  reader loads it without excluding the thread that replaces it; and `placements.h` answers
  `replicas` once a partition rather than once a key, which is what a pass walking a million keys
  asks through.
- **`repository::repository`** is the pure-virtual seam, over tables, records, scans, range deletes and
  the **files a node's share of a table travels in** — `export_records` writes one and `import_records`
  takes one. It is `cluster::partition_set` that says which records a file carries, so the seam includes
  `cluster/partition.h`: what a store is asked to walk for is a set of partitions, and the hashing that
  answers "which partition is this key in" is a pure function of the key. `split_points` is the
  other half of a walk: where a table would be cut up so that several workers can read it at once,
  weighed by the sizes of the files each key starts.
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
- **A key is a partition key and a sort key, composed into one key the store holds.**
  `record::compose_key` is `partition + '\0' + sort`, and the separator is dropped when the sort key
  is empty, so a key of one part is those bytes and no more. It is the **first** zero byte that
  separates (`record::partition_key`, `record::sort_key`), so a key carrying one *is* its two halves —
  `/key/a/b` and `/key/a%00b` are one record, which is why there is no error code for this and
  nothing to reject. The 4 KiB is over the whole composed key. **The separator sorts below every
  other byte**, so a partition key's records are together in the store, in sort key order, and
  before every key the partition key is a prefix of.
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
- **Partitioning is by the partition key alone, never by the table and never by the sort key**, so
  the same key of two tables is in one partition and a record and the records derived from it are
  one hop — and so is every record of one partition key, however many of them there are.
  `cluster::partition_of` takes the composed key and hashes the partition half of it, which is what
  makes every call site right without asking: the export walk, the reconcile pass and the leader
  claim all hand it a key out of the store. The cost is that **a partition key is never split**: it
  is the unit the cluster balances, so one with far more under it than the others is a node with
  more of the table than the others. A write is ordered by the node
  **leading** the key's partition, which writes the copy in every zone and every one of them has to
  take it — all of them at once, so a copy that refuses is a copy the others were written beside
  rather than ahead of; a read goes to one copy — this node when it
  holds one, else the nearest zone's, passing over a node that does not answer, and asking the other
  copies when this node holds nothing for the key. **What a copy that answered says is the answer, a
  404 included**, which rests on a copy being a copy: a node whose rebuild did not read the whole of
  its share answers `node_incomplete` (503) rather than reporting an absence it cannot vouch for, and
  the node reading passes over it as it passes over one that said nothing. `router::is_incomplete` is
  that flag — set from `rebuild::outcome::whole`, cleared by a reconcile pass that settles, and
  reported in `/health` as `incomplete`, because a node holding less than it owns still serves what
  it has and must stay in the load balancer. A table create or delete is **ordered like a write
  and carried to every node**: it is not a record of any partition, so what orders it is the leader
  of `cluster::table_key`, one constant, and from there it goes to every node because a record can
  only be written where its table is. `router::order_schema` is those two hops and the term fence,
  and the leader holds `write_lock(cluster::table_key)` across the whole of one — the tables are
  read, validated against and written under it, so a create is weighed against what the cluster
  held when it was carried out rather than when it arrived, and two creates of one name cannot be
  applied on two nodes at once. A scan is asked of **one zone** — this node's own, since a zone
  holds a copy of the whole keyspace — and merged back into key order, falling back to another zone
  when a node of that one does not answer. A forwarded request carries
  `X-Asyncdb-Forwarded` and is served where it lands, which is what stops two nodes bouncing it. A
  `GET /table/{table}/file` is the same thing by construction: it is answered out of the store it
  landed on and asks the membership nothing.
  `doc/database/cluster.md` is the spec, including what this deliberately does not do (no read
  repair, no replication log, and a write that needs both a leader and every copy).
- **A share is read in several pieces at once, on one thread.** `GET /table/{table}/split?ways=`
  is the node being read saying where to cut its own table up — keys taken from the sizes of the
  files it holds, so the pieces are roughly equal and deliberately approximate. `transfer::walk`
  then asks for every piece in one `send_each`, and hands each answer to a thread of its own that
  takes it into the store while the next round is already being asked for. **Every request a walk
  makes is made on the calling thread**, because a curl handle is `thread_local`: threads made for
  a walk and dropped after it would be a handshake to that node for every walk of every table.
  `from` is the key a piece starts after and `to` is the last key in it, which is what makes the
  pieces a cover — the key one piece stops at is the key the next starts after. `bytes` is the
  whole walk's budget shared out between the pieces, so a share read in eight pieces holds no more
  of itself in memory than one read in one. `transfer::relay` is the one slot between a piece and
  the thread taking its files in, and one slot is what bounds that.
- **A share of a table moves as a file, never a record at a time.** `GET /table/{table}/file?partitions=`
  is what a rebuild and both halves of a reconcile ask for: the partitions are the *asking* node's,
  so the node answering filters by `partition_of` and asks its own membership nothing, and two nodes a
  moment apart still agree on what was sent. The budget — `repository::max_file_bytes`, 64 MiB — is
  what the walk **read** rather than what it wrote, so a node holding a sixth of a zone reads through
  the table once over the whole transfer, and a file the partitions emptied still moves the walk along.
  `X-Asyncdb-Records` and `X-Asyncdb-Next` are what the bytes cannot say: how many records, and base64
  of the key to resume at, absent at the end of the table. Paging a hundred records at a time over HTTP
  is a round trip per hundred, which is not a thing a node holding hundreds of gigabytes finishes.
  **`values=false` is the same walk carrying keys and nothing else**, and the budget counts what it
  read, so a walk that is not reading values covers far more of a table for the same one. That is
  what makes a clear down one question of the node that owns a share rather than one for every key
  in it.
- **Every record carries a version, and it is what decides between two copies.** `record::version` is
  a term and a count: the term is the etcd revision behind the leader's claim, so it rises whenever
  leadership moves or a leader restarts, and the count is that node's own and rises within a term.
  Only a leader issues one, and it travels to the copies in `X-Asyncdb-Count` beside the term they
  already carried, so the copies of one write are one record. `record::compose_value` puts it in
  front of the value in the store, 16 bytes, big endian so the bytes sort as the pair does — which
  is why a file of records carries versions without carrying anything extra, the bytes that move
  being the bytes the store holds. The count comes from `repository::next_count()`, reserved on disk
  a million at a time, so stamping a write costs nothing and a process that restarts carries on
  above every count the one before it issued. **A store written before this refuses to open**: its
  values have no version to tell from their first bytes, and `check_format` says so rather than
  serving what was never written.
- **A file overwrites only what was written before it.** `import_records` keeps a record the store
  holds at a later version and replaces an earlier one, which is what catches up a copy that was
  not there for a write the others took. It is the store that decides and not the caller — the
  RocksDB one walks the incoming file once to see whether anything is held at all, and ingests the
  file as it stands when nothing is. `clear_records` is the other half: it deletes the records a
  file names **unless what is here was written later**, which is a copy the owner has yet to catch
  up on rather than one to hand over, and leaves a key it has nothing for alone, so a pass does not
  write a tombstone for every record it never held. That is why a file of keys alone carries the
  versions in place of the values.
- **Records move when ownership moves, and only then.** A membership change redraws the split inside
  a zone without moving a record, so `reconcile::reconcile` does: it **fetches** what this node now
  owns and holds nothing for, from every other node, and **clears down** what it no longer owns.
  `server::server` runs it on a thread of its own, one tick — `server::reconcile_interval()`, three
  seconds — *after* the membership it saw changed, and one change buys `server::reconcile_attempts`
  (12) passes **of getting nowhere**: a pass that moved records buys them all back, because how many
  passes a share takes is how large the share is and never a count of tries. A membership is a
  moment, and a store that matches the one it was left with is never walked, which is why a test
  naming its own static cluster never runs a pass at all. **What makes the delete safe is that it
  asks first**: a copy is given up only when the node that owns that key *in this node's own zone*
  answers with that key in a file of its own. Not any copy — the record here is this zone's copy, so
  deleting it because another zone has one is a zone left holding nothing — and a node acting on a
  view that is a moment out of date is answered a file without the key in it, keeps the record, and
  asks again next pass. **The clear down is two walks**: the first is local and finds which
  partitions this node holds records it no longer owns in, grouped by the node of its own zone that
  owns them; the second asks each of those nodes for a file of the keys it holds in them. The count
  of what is left when they are done is `deferred`, and a pass with any is not settled.
  **Neither half is safe alone**: clearing down without fetching is a shrink that loses records
  rather than staling them; fetching without clearing down is a store that only grows and a stale
  value waiting for the membership to swing back. Both are best effort and caught like the rebuild,
  because a store that refuses a write must not take the process down from a thread of its own.
  `doc/runbook/rebuild.md` is the page.
- **A pass that moves records is bounded by progress and not by a clock.** `progress::patience` is a
  deadline pushed forward whenever something arrives, and it is what `rebuild::default_seconds` and
  `reconcile::default_seconds` now name: seconds of being answered nothing, not seconds of running.
  A store of a terabyte and a store of a megabyte are the same code, so no wall clock is right for
  both — and a reconcile pass cut off part way is one the pass after it **starts again from the
  beginning**, which is why a short pass is not the cheap way to bound one. What bounds a pass
  instead is the flag `server::server` hands it: a node being shut down waits for the pass in flight,
  so the pass is told to stop rather than kept short enough not to matter.
- **A leader is claimed in etcd, not elected by votes, and which node claims is decided by the
  membership rather than by the race.** `cluster::leader_of` is the node that wins a partition
  across the whole membership under the same rendezvous hashing that chose its copies — which
  makes it the winner in its own zone too, so a leader always holds a copy of what it orders
  writes to, and every node works the same answer out without asking. That node writes
  `/asyncdb/leader/{partition}` with a transaction that only succeeds if nothing created the key,
  on its own membership lease — so a node that stops renewing stops leading. A node claims
  `claims_per_refresh` (64) partitions per pass, from an offset of its own name, so a cold start is
  a pass or two rather than 256 round trips. **A claim outlives the membership it was made under**:
  nothing but a lease takes one away, and a membership change renames the leader of a partition
  without any node losing its lease — so a node gives up the claim on a partition it is no longer
  named for, deleting the key only while it still holds that node's own address and on the second
  pass that finds it gone rather than the first. **Naming and giving up are what make leadership
  follow the membership**: a claim only ever freed by a lease running out is a node that joins a
  healthy cluster leading nothing for as long as it lives, and leadership that settles wherever the
  first race left it is one node ordering the writes of a third of the keyspace and another
  ordering none. Giving one up costs the round trip
  claiming one does and comes out of the same 64. The **term** is the etcd revision that
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
on two ports, each given a `cluster::test_cluster` naming the other — the production routing with the
membership and the leader told to it rather than read from etcd, and a `send_all` that is a real fan
out — so forwarding, table fan-out and merged scans are exercised over real sockets. Both have to stop
the servers they start, and both wait on `server::wait_until_listening` (`test/server/listening.h`)
first: a server binds in its constructor, which is what settles the port a test asks it for, and
listens only in `serve()`, once the store is filled and the node has joined — so a test that started
`serve()` on a thread of its own is racing it.

**Behaviour the server never runs belongs in `test/`, not in `src/`.** A base class body every
implementation in `src/` overrides is production code the suite proves and the binary never
executes: the fan out `cluster::fake_cluster` and `http::fake_client` run one request at a time is
written in each of them and not as a default on the seam, which is why `send_all` is pure virtual
in both `cluster::cluster` and `http::client`. What stays in `src/` is the **seam itself** — the
constructors that take a `cluster::cluster` or an `http::client`, and `server::port()` for the
ephemeral port a test binds — because production reaches those through the other implementation,
and a test that cannot substitute anything is a test against etcd and a fixed port. The line is
whether the code is a way *in* or a second copy of what production already does.

**How much of the store is in memory is `ASYNCDB_MEMORY`,** mebibytes, read by `server::memory_size()`
the way `ASYNCDB_DATA` and `ASYNCDB_THREADS` are read beside it and handed to the repository. It is one
number because it is one thing to size to the instance: three quarters of it is the block cache and a
quarter is the memtables, capped together by `db_write_buffer_size` so that a store of many tables is
not a store of many memtables. Everything else the store is opened with is fixed — a bounded reader
cache, a two level index and a partitioned filter charged to the cache (a node holding a share of a
terabyte is thousands of files, and one index and one filter block apiece is metadata larger than the
machine), and the compression the build was actually linked against, asked for at run time because a
compression this binary does not have is a store it cannot read back.

**The store is one directory, named by `ASYNCDB_DATA`.** `server::data_directory()` reads it and
defaults to `/var/lib/asyncdb`, which the image mounts a volume over — a named one per node in
`docker-compose.yml`, a bind of the host's own in `cloudformation.yaml` — so an instance that is
started again opens what the one before it wrote. The repository opens the directory **as it stands**
rather than something random underneath it, and keeps one subdirectory of its own, `transfer/`, where a
file being exported or imported is built. It is emptied at open: what is in it is a transfer the
process before this one died holding, and there is nothing to resume it with.

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
- `App` wires a `NavPiece` (routes `tables` / `newTable`) and **two side bars**: the left one is
  always there and is `Node`, the health of the instance serving the page — status, whether writes
  are stalled, whether the store is short of what the node owns, the membership it can see and the
  partitions it leads, with a `Zone` a copy; the right one is opened by `Tables` with a
  `TableDetail`. `Node` reads `getHealth()` once when it binds and never polls.
  **What it holds the answer in is a public field**, because a private one is not one datum
  watches: the panel is drawn before the node has answered, and the assignment is what redraws it. `fetchPage` and `DatabaseClient` are injected, so tests pass `() => {}` and
  `FakeDatabaseClient` and then reach into `app.currentPage.datumPiecesCurrentPage` to assert.
- `Tables.#buildGraph` / `#buildRow` do the layered DAG layout: dependency-free tables form row 0, then
  each row takes tables whose dependencies are all already placed (max 6 per row), sorted to sit near
  their dependencies. Positions feed the SVG in `ui/html/table/tables.html`.
- `ui/vite.config.js` aliases `~` to the `ui` root and carries a `rename-datum` plugin working around
  `@datumjs/pieces` still importing the pre-rename `"Datum"` package — needed in both Vite and Vitest.

## Release

Pushing to `master` builds the Docker image and uploads it as a workflow artifact. The workflow is
`.github/workflows/build.yaml`; `github/` at the root holds a byte-identical copy of it and of
`pull-request.yaml` that nothing runs, so a change to one leaves the other stale. That first job,
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
waits for `/health` to name six nodes **and then for those nodes' own `leads` to sum to 256**,
asked of each instance over Run Command because `leads` is a node's own count and the load balancer
answers from one of them — a membership is not yet a cluster that takes writes, and a suite that
starts before the claims settle is answered `no_leader`. It then runs `chaos/validate.sh` and
`chaos/run.sh` over the experiments the matrix names it in `CHAOS_EXPERIMENTS`, and
`make delete-stack`s it again whether they passed or not. **The share carrying `matrix.suites` also
runs the Postman collection, the Playwright journeys and `perf/write.sh` / `perf/read.sh` first**,
before anything has broken its stack. The shares are balanced by measured time — about ten minutes of experiments each out of the
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
is the gate.
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
