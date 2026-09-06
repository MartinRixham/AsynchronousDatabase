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
- name: Tag and push Docker image to ECR
  if: steps.check_version.outputs.publish == 'true'
  run: |
    IMAGE_URI=${{ steps.ecr-login.outputs.registry }}/asyncdb:$VERSION
    docker tag asyncdb:latest $IMAGE_URI
    docker push $IMAGE_URI

- name: Record published version in SSM
  if: steps.check_version.outputs.publish == 'true'
  run: |
    aws ssm put-parameter \
      --name /asyncdb/version \
      --type String \
      --value "$VERSION" \
      --overwrite
```

The tag and the push are **one step**, which they once were not: the `docker
push` lived in a second step of its own and was commented out, so a release
tagged a commit as shipped and left the registry with nothing new in it. The
second-order effect of that is worth remembering because it is what the gate
does when it is lied to: the gate's memory *is* the ECR tag, so a version that
was never pushed is a version `describe-images` keeps failing to find, `publish`
stays `true` on every subsequent push, and the git tag step then fails on a tag
that already exists — the same commit, the same version, going red for a reason
that has nothing to do with the code.

**The `put-parameter` is the other half of publishing**, and it is guarded by the
same `publish`. `/asyncdb/version` is what
[the template resolves at deploy time](/deployment/#parameters), so this line and
not the `docker push` is what decides which tag the next instance to launch will
run. A push that publishes nothing leaves it pointing at the previous release,
which is exactly what [the deploy step in the same
run](/pipeline/#the-job) then stands up.

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
