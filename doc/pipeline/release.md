# The release gate

The last five steps of the job are one decision — has this version been
published already? — and the three things that happen if it has not.

## Reading the version

```yaml
- name: Read version
  id: get_version
  run: |
    VERSION=$(cat version)
    echo "Version: $VERSION"
    echo "VERSION=$VERSION" >> $GITHUB_ENV
```

`version` at the root of the repository is a single line — `0.0.2` at the time of
writing — and it is the only input to everything below. Writing it to
`$GITHUB_ENV` makes it an environment variable for the *following* steps, which
is why each of them also carries a redundant <code v-pre>VERSION: ${{ env.VERSION }}</code> of its
own. The step's `id` is never used; the gate reads the next step's output
instead.

## Asking ECR

```yaml
- name: Check if version exists in ECR
  id: check_version
  run: |
    if aws ecr describe-images \
      --repository-name asyncdb \
      --image-ids imageTag=$VERSION \
      --query 'imageDetails[0].imageTags[0]'
    then
      echo "publish=false" >> $GITHUB_OUTPUT
    else
      echo "publish=true" >> $GITHUB_OUTPUT
    fi
```

The registry is the source of truth: nothing tracks releases anywhere else. The
command's exit status is the whole test, and `--query` only shapes the output it
prints into the log on the way past.

Three steps then hang off `steps.check_version.outputs.publish == 'true'`. Note
what the condition is a test of: **not "is this a new version" but "did that
command fail"**. Anything that makes `describe-images` exit non-zero reads as a
release —

- the repository `asyncdb` does not exist in the account, or not in
  `eu-west-2`;
- the credentials are wrong, expired, or lack `ecr:DescribeImages`;
- ECR is briefly unavailable.

In each of those the run goes green, `docker tag` succeeds locally, and the
commit gets a git tag saying a version shipped that never left the runner.

## Tagging and pushing

```yaml
- name: Tag Docker image for ECR
  if: steps.check_version.outputs.publish == 'true'
  run: |
    IMAGE_URI=${{ steps.ecr-login.outputs.registry }}/asyncdb:$VERSION
    docker tag asyncdb:latest $IMAGE_URI

- name: Push Docker image to ECR
  if: steps.check_version.outputs.publish == 'true'
  run: |
    IMAGE_URI=${{ steps.ecr-login.outputs.registry }}/asyncdb:$VERSION
    # docker push $IMAGE_URI
```

Each step is its own shell, so `IMAGE_URI` is built twice. In the second one it
is built and then not used, because **the `docker push` is commented out**. As
the pipeline stands it publishes nothing: the image exists on the runner, is
tagged for the registry there, and disappears with the runner.

Nothing else in the workflow knows that. The git tag below is still pushed, and
the instances still expect [an image in ECR to
pull](/deployment/#before-the-first-deploy) — so today a release marks a commit
as shipped and leaves the deployment with nothing new to deploy.

There is a second-order effect worth spelling out. The gate's memory *is* the
ECR tag, so with the push commented out the gate can never close on a version
first built after the line was commented: `describe-images` keeps failing to
find it, `publish` stays `true`, and every push to `master` takes the tagging
path again. The final step then tries to push a git tag that already exists and
the run goes red — the same commit, the same version, failing on the second
attempt for a reason that has nothing to do with the code.

## Tagging the commit

```yaml
- name: Tag Git repo with version
  if: steps.check_version.outputs.publish == 'true'
  run: |
    git config user.name "github-actions[bot]"
    git config user.email "github-actions[bot]@users.noreply.github.com"
    git tag "$VERSION"
    git push origin "$VERSION"
```

A lightweight tag on the commit that was built, named exactly as the `version`
file — `0.0.1`, `0.0.2` — with no `v` prefix, matching the image tag character
for character. No credentials are set up here: it works on the ones
`actions/checkout` persisted in `.git/config`, and it needs the `GITHUB_TOKEN` to
have write access to contents.

The checkout is one commit deep and fetches no tags, so `git tag "$VERSION"`
never fails locally on a tag that exists on the remote. The failure, when there
is one, is the push.

## Cutting a release

1. Edit `version`. That is the release: nothing else in the repository names it,
   and the number is not read by the build, only by the workflow.
2. Push to `master`. The image builds, the tests run inside it, ECR is asked, and
   the commit is tagged.
3. Put the `docker push` line back, or the image is not in ECR to be pulled.
4. Bump `VERSION=` in the `LaunchTemplate` user data in `cloudformation.json` and
   `make update-stack`. The stack does not pick the new tag up on its own, and
   even then it reaches an instance only when that instance is
   [replaced](/deployment/#rolling-out-a-new-version).

Leaving `version` alone is a deliberate no-op release: the build still runs in
full, ECR already has the tag, `publish` is `false`, and the three publish steps
skip.
