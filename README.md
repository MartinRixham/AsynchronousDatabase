# Asynchronous Database [![Build Status](https://app.travis-ci.com/MartinRixham/AsynchronousDatabase.svg?branch=master)](https://app.travis-ci.com/MartinRixham/AsynchronousDatabase)
A database for asynchronous data processing

### Build

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

### Run

Run the whole thing with Docker, and open `localhost:8080`:

```bash
docker-compose up
```

### Documentation

The wiki in `doc/` is the API spec. It is a VitePress site with a `package.json` of its own.

```bash
cd doc
npm install
npm run dev           # local preview with hot reload
npm run build         # static build into doc/.vitepress/dist
```
