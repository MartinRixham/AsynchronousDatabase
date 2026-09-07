# The build pipeline

`.github/workflows/build.yaml` is the push side of CI: one workflow, two jobs,
thirty steps, no matrix and no reusable workflow. `build` builds the Docker image
and hands it on as an artifact. `deploy-and-verify` then **publishes it, stands
the whole AWS stack up, runs every test that needs a running server against it,
and deletes it again**. That second job happens only for a version that has not
passed the suite before, which is [the one gate](#the-gate) — and until it has,
each push rewrites the version's image in ECR. The other half
of CI is `.github/workflows/pull-request.yaml`, which is
[two jobs and no AWS at all](#the-pull-request-build).

```
              push to main or master                     concurrency: build-and-push
                        │
                 ┌──────┴──────┐
                 │  checkout   │  actions/checkout@v7, one commit deep
                 └──────┬──────┘
                 ┌──────┴──────┐
                 │docker build │  ← cppcheck, the compile, gtest under valgrind, the UI
                 └──────┬──────┘
            ┌───────────┴───────────┐
            │  tag $VERSION on the  │  the gate — has this version already gone
            │        remote?        │  green all the way through?
            └───────────┬───────────┘
                 ┌──────┴──────┐
                 │ save image  │  docker save | gzip, then upload-artifact, kept a day
                 └──────┬──────┘
                        │
             ═══════════╪═══════════  job boundary: deploy-and-verify
                        │
            ┌───────────┴───────────┐
            │    the same answer    │  needs.build.outputs.verify — the `if:` on the
            │                       │  whole job, and on no step inside it
            └────┬─────────────┬────┘
              yes│             │no
                 │             │
                 │        (whole job
                 │         is skipped)
          ┌──────┴──────┐
          │  AWS login  │  static keys, eu-west-2, then ECR login
          └──────┬──────┘
          ┌──────┴──────┐
          │ load, tag,  │  download-artifact, docker push asyncdb:$VERSION  ← overwrites
          │    push     │  put-parameter /asyncdb/version
          └──────┬──────┘
          ┌──────┴──────┐
          │ mirror etcd │  and put-parameter /asyncdb/etcd
          └──────┬──────┘
          ┌──────┴──────┐
          │create-stack │  and wait for /health to name six nodes
          └──────┬──────┘
          ┌──────┴──────┐
          │   newman    │  the Postman collection
          │  playwright │  the browser journeys
          │    perf     │  write.sh then read.sh
          │    chaos    │  the FIS experiments, which break the stack
          └──────┬──────┘
          ┌──────┴──────┐
          │  git tag    │  $VERSION, pushed to origin — the one
          │             │  place it is tagged, and no `if:`, so
          │             │  only on a green run
          └──────┬──────┘
          ┌──────┴──────┐
          │delete-stack │  if: always() — a red run leaves nothing standing
          └─────────────┘
```

The interesting property of this pipeline is that **one question gates
everything that costs anything, and it is a question about the tests**. Every
push to `master` runs the full image build —
cppcheck, the C++ compile, the gtest suite under valgrind, eslint and vitest —
whether or not anything will be published; a push that does not bump `version` is
not a skipped build but a complete build whose image is thrown away. And the
publish, the API collection, the browser journeys, the load runs and the chaos
suite are all inside the job the gate stands in front of — so the push still
comes **before** the tests that need a stack to run against, which is why the git
tag is at the end of them and not beside the push, and why the image tag they
tested has to be rewritable until then. See [the sharp edges](#sharp-edges).

## The gate

```yaml
- name: Check whether this version has passed already
  id: check_verified
  run: |
    if git ls-remote --exit-code --tags origin "refs/tags/$VERSION" > /dev/null 2>&1
    then
      echo "verify=false" >> $GITHUB_OUTPUT
    else
      echo "verify=true" >> $GITHUB_OUTPUT
    fi
```

<code v-pre>needs.build.outputs.verify == 'true'</code> is the condition on the whole
`deploy-and-verify` job, which is why it is a job and not a run of steps: **a
step added to it is gated by being in it**, rather than by somebody remembering
to repeat the condition on it. That is the only place the condition is written.
[The publish](/pipeline/release#where-the-gate-is) is in that job for the same
reason, and `build` above it has no `if:` on any of its six steps.

The question it asks is deliberately about the **tests** and not about the
registry, and it cannot be the other way round: the stack pulls the image it
tests out of ECR, so the push is the first thing the job does after logging in,
and a published version is not a version that passed. So the tag this reads is
pushed nowhere near the push: [the step that pushes it](#recording-the-pass) is
the last one before the teardown. **A `0.0.2` tag means the version passed**; that it was
published is ECR's business and `/asyncdb/version`'s, and this workflow asks
neither of them a question.

Nothing is held outside the repository to make this work — no parameter, no
bucket, no database. The remote is asked rather than the working tree, because
the checkout is one commit deep and fetches no tags, and `ls-remote` is one round
trip against a ref name that either exists or does not.

The consequences worth knowing:

- **A version that fails is retried, and rebuilt.** Nothing pushes the tag on a
  red run, so the next push on the same version [overwrites the image in
  ECR](/pipeline/release#overwriting-the-tag) with a build of that commit,
  deploys it and runs the suite again. That is what makes a fix pushed on a red
  version the thing the next run actually tests.
- **A version that passed is never stood up again, or republished.** Its image
  tag in ECR is frozen at the build that went green, and the git tag names the
  commit it was built from. Pushes that do not touch
  `version` — a comment, a README, a fix to a test that does not need a cluster —
  cost the image build and nothing else, which is nine `t3.micro`, an ALB and
  roughly $2.80 of FIS action-minutes a push that is never spent.
- **The gate fails open.** `--exit-code` is `0` for a ref that is there and `2`
  for one that is not, but a remote that cannot be reached at all is `128`, and
  the `else` branch takes that too. Unreachable reads as unverified and deploys,
  which is the safe direction: this gate errs towards spending money, never
  towards skipping a suite that should have run.
- **What has passed is `git tag -l`**, and it is readable from a clone by
  anybody, with no AWS credentials at all.
- **To make a version run the suite again without bumping it**, delete the tag:
  `git push --delete origin 0.0.2`. The next push republishes the image from
  whatever `master` is then, so this is a rebuild and not a re-test.
- **The failure direction costs more than money.** Unreachable reads as
  unverified, and unverified publishes as well as deploys — so a version that had
  passed can have its image rewritten by a build of a later commit, which then
  has to pass the whole suite itself to be tagged. Still the safe direction for
  the suite, and not a free one.
- **The gate keys on `version` alone**, and [the etcd mirror](#mirroring-etcd) is
  behind it. So bumping `etcd-version` without bumping `version` mirrors nothing
  and leaves `/asyncdb/etcd` naming the old tag. Bump `version` too, or delete the
  tag.

### Recording the pass

```yaml
- name: Record that this version passed
  run: |
    git config user.name "github-actions[bot]"
    git config user.email "github-actions[bot]@users.noreply.github.com"
    git tag "$VERSION"
    git push origin "$VERSION"
```

It carries **no `if:` of its own**, and that is the whole mechanism: a step with
no condition runs only when every step before it in the job succeeded. The
teardown steps below it are `always()`, so they still run either way, and a step
that is `always()` succeeding does not make a failed job look green to the steps
after it.

It needs no credentials of its own: `actions/checkout` ran in this job too and
persisted them in `.git/config`, and the `GITHUB_TOKEN` needs `contents: write`
for the push. There is no race on the tag either,
because [the concurrency group](#the-trigger) serialises the whole workflow — so
the gate cannot have said "not there" for a run that another run is at this
moment tagging.

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

## The jobs

Two jobs on `ubuntu-latest`. `build` first — six steps, no `if:` on any of them,
and **no AWS credentials at all**:

| Step | Does |
| --- | --- |
| Checkout code | `actions/checkout@v7`, default depth — one commit |
| Build Docker image | `docker build -t asyncdb:latest .` — [the image build](/pipeline/image) |
| Read version | `cat version` into `$GITHUB_ENV` and the `version` output |
| Check whether this version has passed already | `git ls-remote --tags origin refs/tags/$VERSION`, setting the `verify` output — [the gate](#the-gate) |
| Save the image | `docker save asyncdb:latest \| gzip > image.tar.gz` |
| Upload the image | `upload-artifact@v4` as `asyncdb-image`, `compression-level: 0` over an already gzipped tar, `retention-days: 1` — [below](#carrying-the-image) |

Then `deploy-and-verify`, which `needs: build` and runs only when its `verify`
output is `true`. It checks the repository out again — job outputs cross a job
boundary, a workspace and a `$GITHUB_ENV` do not — and takes `$VERSION` from
<code v-pre>needs.build.outputs.version</code>:

| Step | Does |
| --- | --- |
| Checkout code | `actions/checkout@v7` again; the credentials it persists are what lets the tag step push |
| Configure AWS credentials | `aws-actions/configure-aws-credentials@v6` with `AWS_ACCESS_KEY_ID` / `AWS_SECRET_ACCESS_KEY` from repository secrets, region `eu-west-2` |
| Login to Amazon ECR | `aws-actions/amazon-ecr-login@v2`; its `outputs.registry` is the account's registry host, used by the push and by the mirror |
| Download the image | `download-artifact@v4`, which puts `image.tar.gz` back in the workspace |
| Create the asyncdb repository | `aws ecr describe-repositories` or else `create-repository` — [below](#making-the-repositories) |
| Tag and push Docker image to ECR | `docker load`, then `docker tag` and `docker push` — [overwriting the tag](/pipeline/release#overwriting-the-tag) if this version has been published and not yet passed |
| Record published version in SSM | `put-parameter /asyncdb/version` — this is what [the template resolves at deploy time](/deployment/#parameters) |
| Mirror etcd into ECR | [below](#mirroring-etcd) |
| Deploy the stack | `make create-stack`, `wait stack-create-complete`, and the `Url` output into `$GITHUB_ENV`; sets the `created` output every later step keys off |
| Wait for the cluster to come up | `/asyncdb/health` until `.nodes` is **six**, ninety attempts ten seconds apart |
| Install newman, Run the API collection | [the Postman collection](https://github.com/MartinRixham/AsynchronousDatabase/tree/master/api) against `$URL/asyncdb` |
| Install the browser tests, Run the browser tests | Playwright with `ASYNCDB_URL=$URL`, and an `upload-artifact@v4` of the report `if: failure()` |
| Run the load tests | `perf/write.sh` then `perf/read.sh`, eight threads, 250 requests |
| The chaos role | `make create-chaos-stack` — the role FIS assumes, a stack of its own, with its own `created` output |
| Validate the experiment templates | `chaos/validate.sh` — every template created and deleted again, nothing started |
| Run the chaos suite | `chaos/run.sh` — the seven experiments, in order |
| Record that this version passed | `git tag $VERSION` and `git push origin` — [above](#recording-the-pass), and the reason the next push on this version publishes and deploys nothing |
| Tear down the chaos role | `make delete-chaos-stack`, `if: always()` — but only if this run created it |
| Stack events | `make describe-stack`, `if: failure()` |
| What the nodes say for themselves | `if: failure()` — `docker logs` over SSM Run Command and `get-console-output`, per instance, every command best effort so that a diagnosis cannot fail the run |
| Tear down the stack | `make delete-stack`, `if: always()` — but only if this run created it |

The region is a literal in two places — the credentials step here, and, outside
this file, the [user data](/deployment/database#the-launch-template) of both
tiers, which also writes out the registry host. They all say `eu-west-2` and
nothing makes them agree.

## Making the repositories

Neither ECR repository is a thing anybody creates by hand. `asyncdb`'s is made
by the step before the push:

```bash
aws ecr describe-repositories --repository-name asyncdb > /dev/null 2>&1 \
  || aws ecr create-repository --repository-name asyncdb > /dev/null
```

and `etcd`'s by the same two lines inside [the mirror](#mirroring-etcd). Both
are idempotent — the `describe` is the whole test, and on every run after the
first there is nothing to do.

**Where it sits does not matter.** Nothing between it and the push asks the
registry a question, so this step is only what the `docker push` needs, and the
mirror below creates its own repository beside its own push for the same reason.

## Carrying the image

The `docker build` is in `build` and the `docker push` is in `deploy-and-verify`,
and a job gets its own runner, so the image is carried between them as an
artifact:

```yaml
- name: Save the image
  run: docker save asyncdb:latest | gzip > image.tar.gz

- name: Upload the image
  uses: actions/upload-artifact@v4
  with:
    name: asyncdb-image
    path: image.tar.gz
    compression-level: 0
    retention-days: 1
```

`compression-level: 0` because the tar is gzipped already and the action would
otherwise deflate it a second time for nothing. `retention-days: 1` because the
only reader is the next job of the same run; what a released version is kept in
is ECR.

The save and the upload carry **no condition**, so a push on a version that has
passed pays for them and throws them away with the image — a minute or so on top
of a build that is minutes of C++ and valgrind. That is the price of the gate
being written once, on the job.

The alternative is building the image in `deploy-and-verify` instead, and it is
worse: a push that deploys nothing would then run no build and no tests, and the
image build is the only thing [that runs any](#what-ci-does-not-run).

## Mirroring etcd

One step of this workflow is about the *other* tier, and it is here because the
etcd instances pull their image out of this account's registry and never out of
quay.io — [a boot that depends on a third party's registry is a boot that fails
when it does not answer](/deployment/etcd#where-the-image-comes-from). The tag
they run has to be in that registry before the stack that pulls it exists, and
this is what puts it there.

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

It is the other shape — ask ECR whether the tag is there, and push only if it is
not — and it is the right one here, because none of what makes that a poor gate
for asyncdb applies to a tag nothing here builds. A mirrored tag is either the
bytes quay.io published under it or nothing; there is no second commit that would
have produced a better one. Two differences worth knowing:

- **It is in the gated job**, beside the asyncdb push, so it runs exactly when a
  stack is about to pull the tag. `etcd-version` moves for its own reasons and
  usually never, but a bump to it alone mirrors nothing and leaves
  `/asyncdb/etcd` naming the old tag — so bump `version` with it, which is the
  only thing that stands a stack up to run the new one anyway.
- **It creates the repository**, the same way [the step above the version gate
  does for `asyncdb`](#making-the-repositories), and for the same reason: there
  is no reason to make a person create by hand a thing the build knows the name
  of.

The tag itself is written down **once**, in the `etcd-version` file, and
`cloudformation.yaml` never reads a file: the `put-parameter` at the end is the
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
- `perf/` — the load harness likewise: `perf/write.sh` and then `perf/read.sh`
  over the load balancer, failing the build if the cluster answers any of that
  load with anything but a 2xx.
- `chaos/` — the last thing the deploy step's stack sees, and the only thing that
  breaks it on purpose. Each experiment injects one of
  [the runbook's failure modes](/runbook/) with the Fault Injection Service and
  asserts that the cluster behaves and then recovers the way that page says it
  does. It runs after `perf/` so that nothing before it is measuring a cluster
  something else has already broken, and the role FIS assumes is created and
  deleted around it rather than left standing.
- `doc/` — this wiki is never built by either workflow, so a VitePress error or a
  broken link reaches `master` unnoticed.

## Secrets and permissions

Two repository secrets, `AWS_ACCESS_KEY_ID` and `AWS_SECRET_ACCESS_KEY`: static,
long-lived IAM user keys rather than an OIDC role the job assumes. **They are
read by `deploy-and-verify` alone** — `build` configures no credentials, so a
push on a version that has passed reaches AWS not at all. **What they need is
close to everything**, because that job does not only publish an image — it
creates and deletes the stack:

| For | Needs |
| --- | --- |
| The release | `ecr:GetAuthorizationToken`, `ecr:DescribeImages`, and the layer-upload actions behind `docker push` |
| [The repositories](#making-the-repositories) | `ecr:DescribeRepositories` and `ecr:CreateRepository`, for `asyncdb` and for [the mirror](#mirroring-etcd) alike |
| Both parameters | `ssm:PutParameter` on `/asyncdb/*` |
| The deploy | `cloudformation:*` on the stack, plus **every action the template's own resources need** — VPC, subnets, endpoints, security groups, load balancer, auto scaling, and `iam:CreateRole` / `PassRole` for the two instance roles |
| The diagnosis | `ec2:DescribeInstances`, `ec2:GetConsoleOutput`, `ssm:SendCommand` and `ssm:GetCommandInvocation` |

That is a wide key to hold statically in repository secrets, and moving it to an
OIDC role the job assumes is the obvious improvement nobody has made.

[The gate](#the-gate) needs no AWS permission at all: it is a
`git ls-remote` on a remote the checkout already authenticated, which is what
lets it be answered in the job that holds no key.

The workflow declares no `permissions` block, so the `GITHUB_TOKEN` gets the
repository's default, and **one** step needs `contents: write` to push a tag —
[the version tag](#recording-the-pass) at the end of `deploy-and-verify`. If the
default is read-only, a run gets all the way through the suite and then fails on
the push, which is a suite that passed and was not recorded, so the next push
runs it all again.

## Sharp edges

- **Nothing is cached.** No `docker/setup-buildx-action`, no `cache-from`, no
  registry cache: every run compiles the C++ from scratch and memchecks the test
  binary under valgrind, which is the slow part of `cmk verify` by a wide margin.
- **The build is not reproducible.** All three stages are `FROM alpine:latest`,
  the packages come from whatever `apk update` finds today, and cheesemake is
  `git clone`d at its `HEAD`. The same commit built twice a month apart is two
  different images.
- **The publish happens before the tests that need a server**, and has to: the
  stack pulls the image out of ECR. So a Postman assertion, a browser journey or
  a load run that fails does so against a version that is **already published and
  already recorded in `/asyncdb/version`**, and it leaves that standing. What is
  held back is the git tag, and with it the freeze — a red version is a published
  version that the next push overwrites.
  The tests that genuinely gate a release are the ones inside the `docker build`,
  because a failure there fails the job before anything is pushed.
- **The gate fails open.** `git ls-remote` exiting non-zero for any reason
  — the remote is briefly unreachable, the token cannot read it — is
  indistinguishable here from a ref that is not there, and both publishes and
  deploys. That is the safe direction for correctness and the expensive one for
  the bill: the failure mode is a suite that runs when it need not, never one
  that is skipped when it should not be — and, since the gates were merged, a
  released image rewritten by a build that has not passed yet.
- **A green run on a version is the last run on that version.** Once
  `{version}` is on the remote, no later push re-tests it against a cluster,
  however much the working tree has changed underneath — the `deploy-and-verify`
  job is skipped on the version number and nothing else. A change that needs the
  suite needs a `version` bump, which is also the thing that publishes it.
- **A stack left standing by hand fails the run.** The teardown only deletes a
  stack this run created, and `ClusterALB` is a fixed name, so `create-stack`
  fails outright while somebody else's stack exists — and is then correctly left
  alone.
