# Automation

The UI in a real browser, end to end, with [Playwright](https://playwright.dev). What the vitest
suite in [`ui/test`](../ui/test) asserts is the components — this asserts the page they draw against
a database that is really running: the html fragments `fetchPage` pulls in at runtime, the datum
bindings that mutate the DOM, the SVG coordinates the dependency graph is laid out at, and what
`DatabaseClient` really gets back when it sends its requests.

## An instance has to be running

The tests start nothing and stop nothing. They drive an address that serves the whole thing —
the UI, and `/asyncdb` proxied to the database behind it, which is what the nginx of the image does
(`server/server.conf`). `ASYNCDB_URL` is that address, and it defaults to `http://localhost:8080`.

Locally, that is the compose stack in the repository root:

```bash
podman-compose up -d                 # or docker-compose up -d
cd automation
npm install
npx playwright install chromium      # once, to fetch the browser
npm test                             # npm run test:headed to watch it
npm run report                       # the html report of the last run
```

`localhost:8080` is the first of the three instances, and every node answers for every key, so one
of them stands for the cluster. `podman-compose up -d` returns before the database has opened
RocksDB and found etcd, so the run waits up to a minute for `/asyncdb/health` to answer before the
first browser is started, and stops with the address it tried if nothing does.

Against the deployed stack, whose address is the `Url` output of the CloudFormation stack — which is
what the build does on a push to `master`, after the Postman collection and before it tears the
stack down again:

```bash
URL=$(aws cloudformation describe-stacks --stack-name asyncdb \
  --query "Stacks[0].Outputs[?OutputKey=='Url'].OutputValue" --output text)
ASYNCDB_URL=$URL npm test
```

## The database is the test's

There is no stub. `Database` seeds the tables a journey starts on over the same API the UI uses, and
reads back what the page wrote to it, so a create is a real `201` and the error test shows the `409`
the server itself worded.

**A test drops every table it finds, before it runs and after it finishes.** The graph the page
draws is every table the instance holds, so a test that shared the database would be reading another
one's tables out of the SVG — which is also why the tests run one at a time rather than
`fullyParallel`, and why a retry starts from the same empty database the first attempt did. Point
`ASYNCDB_URL` at an instance whose tables can go: the compose stack, or the stack a build stood up
for the run and deletes again. Folder 0 of [`api/`](../api) resets the same way, for the same
reason.

`api/` is the other half of this: the API asserted against a server that is really running, where
this asserts the page.

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
