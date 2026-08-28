# The build pipeline

`.github/workflows/build.yaml` is the whole of CI: one workflow, one job, nine
steps, no matrix and no reusable workflow. It builds the Docker image, asks ECR
whether the version in the `version` file has been published already, and if it
has not, tags the image and tags the commit.

```
              push to main or master
                        │
                 ┌──────┴──────┐
                 │  checkout   │  actions/checkout@v3, one commit deep
                 └──────┬──────┘
                 ┌──────┴──────┐
                 │  AWS login  │  static keys, eu-west-2, then ECR login
                 └──────┬──────┘
                 ┌──────┴──────┐
                 │docker build │  ← the compile and every test CI runs
                 └──────┬──────┘
                 ┌──────┴──────┐
                 │ read version│  the version file, into $GITHUB_ENV
                 └──────┬──────┘
              ┌─────────┴─────────┐
              │ tag in ECR already? │
              └────┬──────────┬─────┘
                yes│          │no
                   │          ├─ docker tag  asyncdb:$VERSION
              (nothing)       ├─ docker push            (commented out)
                              └─ git tag $VERSION && git push origin $VERSION
```

The interesting property of this pipeline is that **the version gate is at the
end, not the beginning**. Every push to `master` runs the full image build —
cppcheck, the C++ compile, the gtest suite under valgrind, eslint and vitest —
whether or not anything will be published at the end of it. A push that does not
bump `version` is not a skipped build; it is a complete build whose result is
thrown away.

## The trigger

```yaml
on:
  push:
    branches: [ main, master ]
```

That is the only trigger. There is no `pull_request` build, so a branch is not
checked by CI until it is merged; no `workflow_dispatch`, so a run cannot be
started by hand from the Actions tab; and no schedule. There is also no
`concurrency` group, so two pushes in quick succession run two builds at once,
racing for the same tag.

## The job

One job, `build-and-push`, on `ubuntu-latest`. Its steps in order:

| Step | Does |
| --- | --- |
| Checkout code | `actions/checkout@v3`, default depth — one commit, and the credentials it persists are what lets the last step push a tag |
| Configure AWS credentials | `aws-actions/configure-aws-credentials@v2` with `AWS_ACCESS_KEY_ID` / `AWS_SECRET_ACCESS_KEY` from repository secrets, region `eu-west-2` |
| Login to Amazon ECR | `aws-actions/amazon-ecr-login@v1`; its `outputs.registry` is the account's registry host, used twice below |
| Build Docker image | `docker build -t asyncdb:latest .` — [the image build](/pipeline/image) |
| Read version | `cat version` into `$GITHUB_ENV` |
| Check if version exists in ECR | `aws ecr describe-images`, setting the `publish` output — [the release gate](/pipeline/release) |
| Tag Docker image for ECR | `docker tag`, only if `publish == 'true'` |
| Push Docker image to ECR | the `docker push` line **is commented out** |
| Tag Git repo with version | `git tag "$VERSION"` and `git push origin "$VERSION"`, only if `publish == 'true'` |

The region is a literal in three places — the credentials step, the
`AWS_DEFAULT_REGION` of the version check, and, outside this file, the
[user data](/deployment/database#the-launch-template) of the instances that pull
the image. They all say `eu-west-2` and nothing makes them agree.

## What CI does not run

The image build is the only thing that runs anything, so what CI covers is
exactly what the `Dockerfile` covers:

- **`cmk verify`** in the builder stage — cppcheck, the compile with `-Werror`,
  the gtest binary, and valgrind over it.
- **`npm test`** in the UI stage — eslint and vitest.

Everything else in the repository is a thing a person runs:

- `api/` — the Postman collection needs a server already up, and no step starts
  one.
- `automation/` — the Playwright journeys are not installed, let alone run.
- `perf/` — the load harness likewise.
- `doc/` — this wiki is never built, so a VitePress error or a broken link
  reaches `master` unnoticed.

## Secrets and permissions

Two repository secrets, `AWS_ACCESS_KEY_ID` and `AWS_SECRET_ACCESS_KEY`: static,
long-lived IAM user keys rather than an OIDC role the job assumes. They need
`ecr:GetAuthorizationToken`, `ecr:DescribeImages` and — once the push is
uncommented — the layer-upload permissions behind `docker push`.

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
- **A red build still tags.** Only the publish steps are guarded by the version
  gate; nothing is guarded by anything else, because the tests live inside the
  `docker build` and a failure there fails the job outright. That is the one
  thing that does stop a release.
- **The version lives in two places.** `version` is what CI tags with, and
  `VERSION=` in `cloudformation.json`'s user data is what the instances pull. CI
  never looks at the template, and the template never looks at `version`.
