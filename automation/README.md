# Automation

The UI in a real browser, end to end, with [Playwright](https://playwright.dev). What the vitest
suite in [`ui/test`](../ui/test) asserts is the components — this asserts the page they draw: the
html fragments `fetchPage` pulls in at runtime, the datum bindings that mutate the DOM, the SVG
coordinates the dependency graph is laid out at, and the requests `DatabaseClient` actually sends.

## Run it

```bash
cd automation
npm install
npx playwright install chromium   # once, to fetch the browser
npm test                          # npm run test:headed to watch it
npm run report                    # the html report of the last run
```

Nothing has to be running first. `playwright.config.js` starts `vite` in [`ui`](../ui) on port 4173
(`UI_PORT` to move it) and stops it again; a dev server already listening there is reused, unless
`CI` is set.

## No database

The database is stubbed in the browser, not in the page: `FakeDatabase` fulfils the
`/asyncdb/table*` requests with `page.route`, so the real `DatabaseClient` is what the test drives —
its URLs, its verbs, the body it sends and the `{ error: { code, message } }` it reads — and no
server, RocksDB or etcd has to be up. The responses are the ones
[`doc/database/tables.md`](../doc/database/tables.md) documents, down to `200` for the same options
again and `409` for different ones, which is what the error test leans on.

`api/` is the other half of this: the API asserted against a server that is really running.

## The tests

Journeys, not features. What a component does with one input, and every branch of it, is the vitest
suite's — these are the paths a user actually walks, end to end over the real page, and the only
errors among them are the ones a user is meant to see.

| File | The journey |
| --- | --- |
| `browseTablesTest.js` | Opening the page on a database that is already there: the graph drawn with a table under the one it depends on, the tables that depend on nothing side by side, and the side bar a table opens and closes |
| `createTableTest.js` | Adding a table: naming it, giving it a dependency, watching it join the graph under that dependency, and finding it in the database and the side bar — and the error a name that is already taken is answered with |

A row of the graph is 180 apart from the next one, so `row(page, name)` in `app.js` turns the `y` a
box is drawn at into the row it is in, and a test asserts that one table sits under another rather
than the coordinate either of them landed on.
