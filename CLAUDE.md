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

## Server architecture

Request flow, one layer per directory under `src/`:

`main.cpp` → `server::server` → `server::session` → `router::router` → `repository::repository` → `table::table`

- **`server::server`** owns the `io_context`, the acceptor and a thread pool sized to
  `hardware_concurrency()`. It also owns the single `rocksdb_repository` and `router`, which are shared
  by reference across all sessions — anything reached from the router must be safe for concurrent use.
  Constructing with port `0` picks a free port and exposes it via `port()`; tests rely on this.
- **`server::session`** is one connection: async read → `handle_request()` → async write, looping while
  keep-alive. It rejects non-GET/POST/HEAD and paths containing `..`, URL-decodes with libcurl, and
  converts any exception escaping the router into a 500 with a JSON `error` body.
- **`router::router`** matches routes by hand (`GET /table?name=…`, `GET /tables`, `POST /table`) and
  returns a `router::response` (status + `boost::json::object`).
- **`repository::repository`** is the pure-virtual seam. `rocksdb_repository` stores tables under keys
  `"TABLE_<name>"` and iterates from `Seek("TABLE")` to list them.

### Domain conventions worth knowing

- `table::table` carries `bool is_valid` plus the JSON body. **Validation failures are values, not
  exceptions**: `table::invalid_table(msg)` returns a table whose `json` is `{ "error": msg }`, which the
  router turns into a 400 and the repository silently refuses to persist. Follow this pattern rather than
  throwing; exceptions are reserved for genuine infrastructure failure (`ERROR(...)` from `src/error.h`
  prefixes file/function/line).
- `table::parse_table` enforces the invariants: non-empty name, name not already taken, and every
  dependency must name an existing table — so the dependency graph can never contain a dangling edge.
- `DEBUG(...)` from `src/log.h` compiles to nothing unless the `LOG` define is `1`; `recipe.json` sets
  `"LOG": "echo 1"` (the define values are shell commands that Cheesemake evaluates).

### Tests

`test/` mirrors `src/`. Unit tests substitute `repository::fake_repository` (an in-memory map) for the
RocksDB implementation. `server_test` is an integration test: it starts a real server on port 0 in a
thread and drives it with libcurl.

**Gotcha:** the RocksDB directory is hardcoded to `/tmp/asyncdb` in `server::server`'s constructor, and
each process opens a *randomly named* subdirectory of it, so data does not survive a restart and stale
directories accumulate. Tests `remove_all("/tmp/asyncdb/")` in `SetUp`.

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
