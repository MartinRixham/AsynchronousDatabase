# The image build

```yaml
- name: Build Docker image
  run: docker build -t asyncdb:latest .
```

One line, no build args, no `--platform`, no buildx and no cache. Everything CI
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

```dockerfile
FROM alpine:latest AS builder
RUN apk add bash build-base git jq openssl cppcheck rocksdb-dev \
    boost-dev gtest-dev curl-dev valgrind gcovr py3-pygments
RUN git clone https://github.com/martinrixham/cheesemake
COPY src/ src/
COPY test/ test/
COPY recipe.json valgrind.chevre ./
RUN cheesemake/cheesemake verify
```

`verify` runs every phase up to it, so this single line is the whole check:
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

```dockerfile
FROM alpine:latest AS ui
ENV CI=1
COPY ui .
RUN rm -rf node_modules
RUN npm install
RUN npm test
RUN npm run build
```

`CI=1` is what stops vitest sitting in watch mode. The `rm -rf node_modules` is
belt and braces over `.dockerignore` — the copy should never have brought any —
and `npm install` rather than `npm ci` means the lockfile is a suggestion here.

`npm test` is eslint and vitest, and like the builder stage it fails the whole
`docker build` when it fails. `npm run build` emits `dist`, which the last stage
takes and nothing else does.

## Stage 3 — what ships

```dockerfile
FROM alpine:latest
RUN apk add libstdc++ rocksdb curl nginx
COPY server/server.conf /etc/nginx/http.d/default.conf
COPY --from=ui /ui/dist /usr/share/nginx/html
COPY server/404.html server/50x.json /usr/share/nginx/html/
COPY --from=builder /build/bin/asyncdb .
CMD nginx & ./asyncdb
EXPOSE 80 8080
```

Four runtime packages, and neither Boost nor a compiler among them: the image is
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
