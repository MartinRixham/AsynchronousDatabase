# The image build

The workflow's `Build Docker image` step is `docker build -t asyncdb:latest .`:
one line, no build args, no `--platform`, no buildx and no cache. Everything CI
actually checks happens inside it, because the `Dockerfile` is three stages and
two of them are a test run.

## The context

`.dockerignore` keeps four things out of what is uploaded to the daemon:

| Excluded | Because |
| --- | --- |
| `**/node_modules`, `ui/dist` | the UI stage runs `npm install` and `npm run build` itself |
| `build/` | the builder stage compiles from source, and cheesemake's hashes in `build/hashes` would make an incremental build of somebody's laptop state |
| `.git`, `.github`, `.vscode`, `*.pem` | no stage reads them |

`.git` being excluded is worth noticing: **nothing inside the image build can
see the version**. The `Dockerfile` never copies the `version` file either, so
the built image carries no identifier of its own — the ECR tag is the only place
the version exists, and it is applied [afterwards](/pipeline/release), by the
workflow.

## Stage 1 — the server

`builder` installs the toolchain, clones cheesemake, copies the sources, the
tests, `recipe.json` and `valgrind.chevre`, and runs `cheesemake verify`.
`verify` runs every phase up to it, so that single line is the whole check:
`cppcheck --enable=style`, the compile with `-Wall -Werror`, the gtest binary,
and valgrind over that binary. **A failing test fails `docker build`, which
fails the job** — that is how the pipeline gates on the tests without having a
test step.

Two details of the copy list matter. `valgrind.chevre` is copied because it is
this project's own plugin overriding cheesemake's, which would otherwise memcheck
`build/bin/asyncdb` — a server that serves until it is signalled, so `verify`
would never return. And `boost-dev` is a build-stage package with no counterpart
in the runtime stage, which is the packaging half of the
header-only Boost rule: every Boost header the server uses needs no linking, so
none of it has to exist at runtime.

The clone is unpinned. cheesemake's `HEAD` is a moving target and the build
follows it.

## Stage 2 — the UI

`ui` copies `ui/`, removes any `node_modules`, and runs `npm install`, `npm test`
and `npm run build` with `CI=1` set.

`CI=1` is what stops vitest sitting in watch mode. The `rm -rf node_modules` is
belt and braces over `.dockerignore` — the copy should never have brought any —
and `npm install` rather than `npm ci` means the lockfile is a suggestion here.

`npm test` is eslint and vitest, and like the builder stage it fails the whole
`docker build` when it fails. `npm run build` emits `dist`, which the last stage
takes and nothing else does.

## Stage 3 — what ships

The last stage installs `libstdc++ rocksdb curl nginx`, copies in the nginx
configuration, the built UI, the error pages and the binary, and starts both
processes. Four runtime packages, and neither Boost nor a compiler among them: the image is
alpine plus the shared libraries the binary actually needs. Adding a compiled
Boost library to the server would mean adding a package here as well, which is
the practical reason the rule holds.

nginx serves the built UI on port 80 and reverse-proxies `/asyncdb/*` to the
binary on 8080 — which is why the UI's `DatabaseClient` uses relative
`asyncdb/...` URLs, and why the deployed instances
[publish both ports](/deployment/database#the-launch-template): 80 for a browser
coming through the load balancer, 8080 for the other nodes of the cluster
forwarding a key they do not own.

`CMD nginx & ./asyncdb` puts the two processes under a shell with no init and no
supervision. `./asyncdb` is what the container's lifetime follows; nginx dying
leaves a container that is up and serving nothing, and the health check on the
load balancer is what eventually notices.
