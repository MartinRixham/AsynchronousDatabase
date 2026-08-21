---
layout: home

hero:
  name: asyncdb
  text: A database for asynchronous data processing
  tagline: A C++20 HTTP/JSON server over RocksDB, with a single-page UI that draws your tables and their dependencies as a DAG.
  actions:
    - theme: brand
      text: Overview
      link: "#overview"
    - theme: alt
      text: GitHub
      link: https://github.com/MartinRixham/AsynchronousDatabase

features:
  - title: HTTP and JSON
    details: Boost.Beast serves an asynchronous, keep-alive HTTP API on a thread pool sized to the machine. Every request and response is plain JSON.
  - title: Durable storage
    details: Tables are persisted in RocksDB behind a small repository seam, so the storage engine can be swapped without touching the routing or domain layers.
  - title: A visible dependency graph
    details: The vanilla-JS UI lays the tables out in rows — dependency-free tables first — and renders the edges between them as an SVG graph.
---

## Overview

**asyncdb** is a database for asynchronous data processing. A *table* is a named node with a list of
dependencies on other tables; together they form a directed acyclic graph that describes how data flows
through the system. The server keeps that graph consistent, and the UI draws it.

The project is in two halves:

- a **C++20 server** built on [Boost.Beast](https://www.boost.org/doc/libs/release/libs/beast/) and
  [RocksDB](https://rocksdb.org/), and
- a **single-page UI** written in vanilla JavaScript on top of
  [@datumjs/datum](https://www.npmjs.com/package/@datumjs/datum).

They are shipped together in one Docker image: nginx serves the built UI and reverse-proxies `/asyncdb/*`
to the server binary.

### The API

Three routes, all JSON:

| Method | Route                | Description                                   |
| ------ | -------------------- | --------------------------------------------- |
| `GET`  | `/table?name=<name>` | Fetch one table, or `404` if it does not exist |
| `GET`  | `/tables`            | List every table                              |
| `POST` | `/table`             | Create a table                                |

A table is posted as `{ "name": "orders", "dependencies": ["customers"] }`. Creation is rejected with a
`400` and an `{ "error": ... }` body if the name is empty, if the name is already taken, or if any
dependency does not name an existing table — so the graph can never contain a dangling edge.

### How a request travels

Each layer of the server lives in its own directory and namespace under `src/`:

```
main.cpp → server::server → server::session → router::router → repository::repository → table::table
```

`server::server` owns the `io_context`, the acceptor and the thread pool, and shares a single repository
and router across every connection. `server::session` is one connection: it reads asynchronously, hands
the request to the router, writes the response and loops while the connection is kept alive.
`router::router` matches the routes above by hand. `repository::repository` is a pure-virtual seam whose
RocksDB implementation stores tables under `TABLE_<name>` keys.

Validation failures are treated as values rather than exceptions: an invalid table carries an `error`
object that the router turns into a `400`. Exceptions are reserved for genuine infrastructure failure.

### Getting started

The server is built with [Cheesemake](https://github.com/martinrixham/cheesemake), driven by
`recipe.json`. Running a phase runs every phase before it.

```bash
cmk verify          # cppcheck, compile, run the tests, link build/bin/asyncdb
cmk test            # stop after the tests
cmk run             # build, then run the server
```

The UI is built with Vite:

```bash
cd ui
npm test            # eslint and vitest
npm start           # dev server
npm run build       # emits ui/dist
```

Or run the whole thing with Docker, and open `localhost:8080`:

```bash
docker-compose up
```

### These docs

This wiki is a VitePress site living in `doc/`.

```bash
cd doc
npm install
npm run docs:dev      # local preview with hot reload
npm run docs:build    # static build into doc/.vitepress/dist
```
