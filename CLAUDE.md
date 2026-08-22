# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

A C++20 HTTP/JSON database server for asynchronous data processing (Boost.Beast + RocksDB), plus a
vanilla-JS single-page UI that draws the tables and their dependencies as a DAG.

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

### Running the whole thing

`docker-compose up`, then the UI is on `localhost:8080`. The image runs nginx on port 80 serving
`ui/dist` and reverse-proxying `/asyncdb/*` to the `asyncdb` binary on `localhost:8080`
(`server/server.conf`); that is why `DatabaseClient` uses relative `asyncdb/...` URLs. The etcd service
in the compose file is groundwork for clustering and is not yet wired into the server.

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
| libcurl | Percent-decoding in `url::decode`, and driving the server in `server_test` |

**Prefer a header-only Boost to writing it again** — that is what replaced a hand-rolled UTF-8 decoder
with `boost::locale::utf`. Two deliberate exceptions:

- **Boost.URL** would replace most of `url`, and it handles `%2F`, `%00` and invalid UTF-8 correctly.
  It is not used because `<boost/url/src.hpp>` is discontinued as of Boost 1.90 (it is an `#error`), so
  it would have to be linked; and because `params()` decodes `+` as a space, which would quietly
  corrupt any key or `from`/`to` bound containing a literal `+`.
- **base64** in `scan.cpp` is written out rather than taken from `boost::beast::detail::base64`, which
  works but is Beast's private namespace.

On the other side, the UI's one dependency of substance is
[@datumjs/datum](https://www.npmjs.com/package/@datumjs/datum) — see [UI architecture](#ui-architecture)
— and the wiki in `doc/` is a VitePress site with its own `package.json`.

## Server architecture

The API the server implements is the one the wiki describes in `doc/database/` — that is the spec,
and `doc/database/reference.md` is the list of endpoints, error codes and limits.

Request flow, one layer per directory under `src/`:

`main.cpp` → `server::server` → `server::session` → `router::router` → `repository::repository` →
`table::table` / `record::record` / `scan::range`

- **`server::server`** owns the `io_context`, the acceptor and a thread pool sized to
  `hardware_concurrency()`. It also owns the single `rocksdb_repository` and `router`, which are shared
  by reference across all sessions — anything reached from the router must be safe for concurrent use.
  Constructing with port `0` picks a free port and exposes it via `port()`; tests rely on this.
- **`server::session`** is one connection: async read → `handle_request()` → async write, looping while
  keep-alive. It rejects methods other than GET/HEAD/PUT/DELETE and any path segment that is `..`,
  builds a `router::request`, and turns an escaping `repository::storage_error` into its own status and
  any other exception into a 500 `storage_error`. HEAD answers the headers of the GET with no body and
  the length the body would have had.
- **`url`** splits the target at its unencoded slashes *before* percent-decoding each segment, so a key
  containing `/`, `?` or a zero byte stays one segment. Query values are decoded the same way.
- **`router::router`** matches routes by hand — `/health`, `/table`, `/table/{table}`,
  `/table/{table}/key` and `/table/{table}/key/{key}` — and returns a `router::response` (status,
  content type, and either a `boost::json::object` or the raw text of a value). `router/api_error.cpp`
  is the one place a documented error code is mapped to a status.
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
- `table::parse_table` enforces the invariants: a name of 1–64 characters of `[a-z0-9_-]` that is not
  `default`, and every dependency must name an existing table — so the dependency graph can never
  contain a dangling edge. `PUT /table/{table}` is idempotent: the same options again are `200`, and
  different ones are `409`, which is why a cycle cannot be built.
- `record::parse_record` enforces the limits (4 KiB of key, 16 MiB of value) and that a key is valid
  UTF-8. A value is never looked at — every string is a value, and the empty one is told from a missing
  key by the status code, which is why `read_record` returns a `std::optional`.
- `scan::range` is the parsed query of a scan or a range delete, and a cursor is base64 of
  `{ "k": last key, "s": instance }`; the instance is what makes a cursor this instance did not issue
  refusable.
- `DEBUG(...)` from `src/log.h` compiles to nothing unless the `LOG` define is `1`; `recipe.json` sets
  `"LOG": "echo 1"` (the define values are shell commands that Cheesemake evaluates).

### Tests

`test/` mirrors `src/`. Unit tests substitute `repository::fake_repository` (an in-memory map) for the
RocksDB implementation. `server_test` is an integration test: it starts a real server on port 0 in a
thread and drives it with libcurl.

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
exist in ECR**, pushes it and git-tags the commit. Bump `version` to cut a release; leaving it unchanged
makes CI a no-op publish. AWS infrastructure lives in `cloudformation.json`, driven by the `Makefile`
(`make create-stack` / `update-stack` / `delete-stack`).
