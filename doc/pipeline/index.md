# The build pipeline

`.github/workflows/build.yaml` is the push side of CI: one workflow, one job,
twenty-two steps, no matrix and no reusable workflow. It builds the Docker image,
asks ECR whether the version in the `version` file has been published already and
publishes it if it has not — and then **stands the whole AWS stack up, runs every
test that needs a running server against it, and deletes it again**. The other
half of CI is `.github/workflows/pull-request.yaml`, which is
[two jobs and no AWS at all](#the-pull-request-build).

```
              push to main or master                     concurrency: build-and-push
                        │
                 ┌──────┴──────┐
                 │  checkout   │  actions/checkout@v7, one commit deep
                 └──────┬──────┘
                 ┌──────┴──────┐
                 │  AWS login  │  static keys, eu-west-2, then ECR login
                 └──────┬──────┘
                 ┌──────┴──────┐
                 │docker build │  ← cppcheck, the compile, gtest under valgrind, the UI
                 └──────┬──────┘
                 ┌──────┴──────┐
                 │  make repo  │  create-repository asyncdb if it is not there
                 └──────┬──────┘
              ┌─────────┴─────────┐
              │ tag in ECR already? │  the version file
              └────┬──────────┬─────┘
                yes│          │no
                   │          ├─ docker push asyncdb:$VERSION
              (nothing)       ├─ put-parameter /asyncdb/version
                   │          └─ git tag $VERSION && git push origin $VERSION
                   └────┬─────┘
                 ┌──────┴──────┐
                 │ mirror etcd │  unconditional — the version gate does not reach it
                 └──────┬──────┘
                 ┌──────┴──────┐
                 │create-stack │  and wait for /health to name six nodes
                 └──────┬──────┘
                 ┌──────┴──────┐
                 │   newman    │  the Postman collection
                 │  playwright │  the browser journeys
                 │    perf     │  write.sh then read.sh
                 └──────┬──────┘
                 ┌──────┴──────┐
                 │delete-stack │  if: always() — a red run leaves nothing standing
                 └─────────────┘
```

The interesting property of this pipeline is that **the version gate is in the
middle, and everything that could fail the release is on both sides of it**.
Every push to `master` runs the full image build — cppcheck, the C++ compile, the
gtest suite under valgrind, eslint and vitest — whether or not anything will be
published; a push that does not bump `version` is not a skipped build but a
complete build whose image is thrown away. And the API collection, the browser
journeys and the load runs all happen **after** the push and the git tag, against
a stack the run created for them. See [the sharp edges](#sharp-edges).

## The trigger

```yaml
on:
  push:
    branches: [ main, master ]

concurrency:
  group: build-and-push
  cancel-in-progress: false
```

That is the only trigger of this workflow: no `workflow_dispatch`, so a run
cannot be started by hand from the Actions tab, and no schedule. The
`concurrency` group is what stops two pushes in quick succession racing for the
same tag and, worse, for the same CloudFormation stack — `ClusterALB` is a fixed
name, so there can only ever be one. `cancel-in-progress` is **false**
deliberately: a cancelled run is one whose `delete-stack` never runs.

### The pull request build

`.github/workflows/pull-request.yaml` is the other workflow, on `pull_request`
into `main` or `master`. Two jobs, in parallel, and neither touches AWS:

| Job | Does |
| --- | --- |
| `server` | `docker build --target builder .` — cppcheck, the compile, gtest under valgrind |
| `ui` | `docker build --target ui .` — eslint, vitest and the Vite build |

So a branch **is** checked before it is merged; what it is not checked with is
anything that needs a server, because nothing is deployed for a pull request.

## The job

One job, `build-and-push`, on `ubuntu-latest`. Its steps in order:

| Step | Does |
| --- | --- |
| Checkout code | `actions/checkout@v7`, default depth — one commit, and the credentials it persists are what lets the tag step push |
| Configure AWS credentials | `aws-actions/configure-aws-credentials@v6` with `AWS_ACCESS_KEY_ID` / `AWS_SECRET_ACCESS_KEY` from repository secrets, region `eu-west-2` |
| Login to Amazon ECR | `aws-actions/amazon-ecr-login@v2`; its `outputs.registry` is the account's registry host, used by the push and by the mirror |
| Build Docker image | `docker build -t asyncdb:latest .` — [the image build](/pipeline/image) |
| Read version | `cat version` into `$GITHUB_ENV` |
| Create the asyncdb repository | `aws ecr describe-repositories` or else `create-repository` — [below](#making-the-repositories) |
| Check if version exists in ECR | `aws ecr describe-images`, setting the `publish` output — [the release gate](/pipeline/release) |
| Tag and push Docker image to ECR | `docker tag` and `docker push`, only if `publish == 'true'` |
| Record published version in SSM | `put-parameter /asyncdb/version`, only if `publish == 'true'` — this is what [the template resolves at deploy time](/deployment/#parameters) |
| Tag Git repo with version | `git tag "$VERSION"` and `git push origin "$VERSION"`, only if `publish == 'true'` |
| Mirror etcd into ECR | [below](#mirroring-etcd) — unconditional, and the one publish the version gate does not guard |
| Deploy the stack | `make create-stack`, `wait stack-create-complete`, and the `Url` output into `$GITHUB_ENV`; sets the `created` output every later step keys off |
| Wait for the cluster to come up | `/asyncdb/health` until `.nodes` is **six**, ninety attempts ten seconds apart |
| Install newman, Run the API collection | [the Postman collection](https://github.com/MartinRixham/AsynchronousDatabase/tree/master/api) against `$URL/asyncdb` |
| Install the browser tests, Run the browser tests | Playwright with `ASYNCDB_URL=$URL`, and an `upload-artifact@v4` of the report `if: failure()` |
| Run the load tests | `perf/write.sh` then `perf/read.sh`, eight threads, 250 requests |
| Stack events | `make describe-stack`, `if: failure()` |
| What the nodes say for themselves | `if: failure()` — `docker logs` over SSM Run Command and `get-console-output`, per instance, every command best effort so that a diagnosis cannot fail the run |
| Tear down the stack | `make delete-stack`, `if: always()` — but only if this run created it |

The region is a literal in two places — the credentials step here, and, outside
this file, the [user data](/deployment/database#the-launch-template) of both
tiers, which also writes out the registry host. They all say `eu-west-2` and
nothing makes them agree.

## Making the repositories

Neither ECR repository is a thing anybody creates by hand. `asyncdb`'s is made
by one step above the version gate:

```bash
aws ecr describe-repositories --repository-name asyncdb > /dev/null 2>&1 \
  || aws ecr create-repository --repository-name asyncdb > /dev/null
```

and `etcd`'s by the same two lines inside [the mirror](#mirroring-etcd). Both
are idempotent — the `describe` is the whole test, and on every run after the
first there is nothing to do.

**Where it sits matters more than that it exists.** It is not enough to create
the repository before the `docker push`, because the step in between is
[the release gate](/pipeline/release), and that gate is a test of whether
`describe-images` exited non-zero. A missing repository does not make it answer
"no such tag"; it makes it fail, which the gate reads as a release. Creating the
repository first is what gives the question an answer, and it is why this step
is above the gate rather than beside the push.

## Mirroring etcd

One step of this workflow is about the *other* tier, and it is here because
[the etcd instances are in a private subnet with no route to
quay.io](/deployment/network#why-the-instances-are-private). The tag they run has
to be in this account's registry before the stack that pulls it exists, and this
is what puts it there.

```bash
ETCD_VERSION=$(cat etcd-version)

aws ecr describe-repositories --repository-name etcd > /dev/null 2>&1 \
  || aws ecr create-repository --repository-name etcd > /dev/null

if aws ecr describe-images --repository-name etcd --image-ids imageTag=$ETCD_VERSION > /dev/null 2>&1
then
  echo "etcd:$ETCD_VERSION is mirrored already."
else
  docker pull quay.io/coreos/etcd:$ETCD_VERSION
  docker tag  quay.io/coreos/etcd:$ETCD_VERSION $REGISTRY/etcd:$ETCD_VERSION
  docker push $REGISTRY/etcd:$ETCD_VERSION
fi

aws ssm put-parameter --name /asyncdb/etcd --type String --value "$ETCD_VERSION" --overwrite
```

It is the same shape as [the release gate](/pipeline/release) one tag down —
ask ECR whether it is there, and push only if it is not — with two differences
worth knowing:

- **It is not guarded by `publish`.** The `version` gate decides whether *this
  repository's* image is published; `etcd-version` moves for its own reasons and
  usually never, so the two are asked separately. A push that publishes nothing
  still leaves the registry and `/asyncdb/etcd` correct, which is what the
  `make create-stack` two steps later depends on.
- **It creates the repository**, the same way [the step above the version gate
  does for `asyncdb`](#making-the-repositories), and for the same reason: there
  is no reason to make a person create by hand a thing the build knows the name
  of.

The tag itself is written down **once**, in the `etcd-version` file, and
`cloudformation.json` never reads a file: the `put-parameter` at the end is the
bridge, and `EtcdVersion` resolves `/asyncdb/etcd` at deploy time exactly as
`Version` resolves `/asyncdb/version`. The one other place the version appears is
`docker-compose.yml`, which pulls from quay.io directly because a laptop can.

There **was** a second workflow behind all this — `.github/workflows/ami.yaml`,
a manual Packer bake of one AMI per tier, whose reason to exist was that an etcd
instance had no way to get its container. Mirroring is the better answer to that:
it takes quay.io off the boot path just as the bake did, and it does so on the
pipeline that already runs rather than on one somebody has to remember. What was
left of the bake once the etcd image came out of it was a `dnf -y update` and a
`mkdir`, against a base image AWS republishes patched, so both tiers now launch
from [that base image directly](/deployment/#parameters).

## What CI does not run

The image build is the only thing that runs anything, so what CI covers is
exactly what the `Dockerfile` covers:

- **`cmk verify`** in the builder stage — cppcheck, the compile with `-Werror`,
  the gtest binary, and valgrind over it.
- **`npm test`** in the UI stage — eslint and vitest.

Everything else in the repository is a thing a person runs:

- `api/` and `automation/` — both need a server already up, and on a push to
  `master` the deploy step is what gives them one: the collection and then the
  Playwright journeys run against the stack the build stood up, and it is deleted
  again whether they passed or not. On a pull request nothing runs them, because
  [nothing is deployed](#the-pull-request-build).
- `perf/` — the load harness likewise, and on `master` it is the last thing the
  deploy step's stack sees: `perf/write.sh` and then `perf/read.sh` over the load
  balancer, failing the build if the cluster answers any of that load with
  anything but a 2xx.
- `doc/` — this wiki is never built by either workflow, so a VitePress error or a
  broken link reaches `master` unnoticed.

## Secrets and permissions

Two repository secrets, `AWS_ACCESS_KEY_ID` and `AWS_SECRET_ACCESS_KEY`: static,
long-lived IAM user keys rather than an OIDC role the job assumes. **What they
need is close to everything**, because the job does not only publish an image —
it creates and deletes the stack:

| For | Needs |
| --- | --- |
| The release | `ecr:GetAuthorizationToken`, `ecr:DescribeImages`, and the layer-upload actions behind `docker push` |
| [The repositories](#making-the-repositories) | `ecr:DescribeRepositories` and `ecr:CreateRepository`, for `asyncdb` and for [the mirror](#mirroring-etcd) alike |
| Both parameters | `ssm:PutParameter` on `/asyncdb/*` |
| The deploy | `cloudformation:*` on the stack, plus **every action the template's own resources need** — VPC, subnets, endpoints, security groups, load balancer, auto scaling, and `iam:CreateRole` / `PassRole` for the two instance roles |
| The diagnosis | `ec2:DescribeInstances`, `ec2:GetConsoleOutput`, `ssm:SendCommand` and `ssm:GetCommandInvocation` |

That is a wide key to hold statically in repository secrets, and moving it to an
OIDC role the job assumes is the obvious improvement nobody has made.

The workflow declares no `permissions` block, so the `GITHUB_TOKEN` gets the
repository's default, and the last step needs `contents: write` to push a tag.
If the default is read-only, the run gets as far as tagging and then fails on
the push.

## Sharp edges

- **Nothing is cached.** No `docker/setup-buildx-action`, no `cache-from`, no
  registry cache: every run compiles the C++ from scratch and memchecks the test
  binary under valgrind, which is the slow part of `cmk verify` by a wide margin.
- **The build is not reproducible.** All three stages are `FROM alpine:latest`,
  the packages come from whatever `apk update` finds today, and cheesemake is
  `git clone`d at its `HEAD`. The same commit built twice a month apart is two
  different images.
- **The release happens before the tests that need a server.** The push, the
  `put-parameter` and the git tag are all four steps ahead of `create-stack`, so
  a Postman assertion, a browser journey or a load run that fails does so against
  a version that is **already published and already recorded in
  `/asyncdb/version`**. It fails the build, and it leaves the release standing.
  The tests that genuinely gate a release are the ones inside the `docker build`,
  because a failure there fails the job before anything is pushed.
- **A no-op publish still deploys.** `create-stack` is not guarded by the version
  gate, so a push that bumps nothing stands up a stack running whatever
  `/asyncdb/version` already said — the previous release, not the working tree —
  and runs the whole suite against it. That is usually what you want and is never
  what the diff in front of you says.
- **A stack left standing by hand fails the run.** The teardown only deletes a
  stack this run created, and `ClusterALB` is a fixed name, so `create-stack`
  fails outright while somebody else's stack exists — and is then correctly left
  alone.
