# Publishing

Most of `build-and-push` is one decision — has this version passed the suite
already? — and the two things that happen if it has not.

There used to be two decisions here, and they were easy to confuse. This page
asked ECR whether the image was published; [the deploy
gate](/pipeline/#the-gate) asked a git tag whether the version had been
tested. **They are now the same gate**, read in two places: the image is
published for a version that has not passed, and republished on every push until
it does.

## Why the registry could not be the gate

The obvious gate is the registry — is `asyncdb:0.0.2` there already? — and it was
this one for a long time:

```yaml
if aws ecr describe-images \
  --repository-name asyncdb \
  --image-ids imageTag=$VERSION
then
  echo "publish=false" >> $GITHUB_OUTPUT
```

It could never have been the *deploy* gate, because [the stack pulls the image it
tests out of ECR](/deployment/#parameters): the push has to come first, so a
published version is not yet a version that passed, and skipping the suite for
anything already in the registry would skip it for a version whose first run went
red.

What is less obvious is that it was a poor **publish** gate too. It froze the
image at the first commit that carried the version, so on a red version:

- the fix pushed at the next commit was built by CI and thrown away;
- the stack the suite then ran against pulled the **first** commit's image;
- and a green run recorded a pass for code that had never been in the image it
  tested.

Which is why the fix is [a mutable tag](#overwriting-the-tag) rather than a
second gate.

## Overwriting the tag

`create-repository` sets no `--image-tag-mutability`, so the repository is
`MUTABLE`, which is the default — and nothing here ever wanted otherwise.
`asyncdb:0.0.2` is therefore rewritten by every push that carries version `0.0.2`
and has not passed yet, and the last one to be written is the one the suite runs
against and the one the git tag names.

**The window in which a version tag moves is exactly the window before it
passes.** The moment the tag is on the remote, the gate closes and the image is
frozen with it: nothing republishes `0.0.2`, and `docker pull asyncdb:0.0.2` is
the build that went green. What that asks of everybody is one thing — **do not
pull a version that has no git tag**, because it is a version still being worked
on, and neither its bytes nor its number mean anything yet.

## The gate in this job

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

The same step, the same output, and the same `ls-remote` that
[gates the whole `deploy-and-verify` job](/pipeline/#the-gate) — see there
for what it reads and how it fails. Two steps in *this* job hang off it as well:

```yaml
- name: Tag and push Docker image to ECR
  if: steps.check_verified.outputs.verify == 'true'
  run: |
    IMAGE_URI=${{ steps.ecr-login.outputs.registry }}/asyncdb:$VERSION
    docker tag asyncdb:latest $IMAGE_URI
    docker push $IMAGE_URI

- name: Record published version in SSM
  if: steps.check_verified.outputs.verify == 'true'
  run: |
    aws ssm put-parameter \
      --name /asyncdb/version \
      --type String \
      --value "$VERSION" \
      --overwrite
```

The tag and the push are **one step**, which they once were not: the `docker
push` lived in a second step of its own and was commented out, so a release
recorded a version that never left the runner. It is worth keeping in mind
because the gate no longer notices: it asks git, not the registry, so a push that
silently does nothing is a version the suite tests as whatever ECR happens to
hold under that tag. If that is nothing, the instances have no container, the
cluster never reaches six nodes and the run fails on the wait — which is the
detection, and it is a coarse one.

**The `put-parameter` is the other half of publishing.** `/asyncdb/version` is
what [the template resolves at deploy time](/deployment/#parameters), so this line
and not the `docker push` is what decides which tag the next instance to launch
will run — including the instances `deploy-and-verify` stands up three steps
later, which is how the suite comes to test what this job just built.

## Making the repository

Neither ECR repository is a thing anybody creates by hand, and `asyncdb`'s is made
by the step above the gate — `describe-repositories` or else `create-repository`,
[as the mirror does for `etcd`](/pipeline/#making-the-repositories). It was once
part of the ECR gate itself, and had to be: a missing repository fails
`describe-images`, which that gate read as "not published yet". Now that nothing
asks the registry a question, it is only what the `docker push` needs.

## Cutting a release

1. Edit `version`. That is the release: nothing else in the repository names it,
   and the number is not read by the build, only by the workflow.
2. Push to `master`. The image builds, the tests inside it run, and the image is
   published to ECR and named in `/asyncdb/version`.
3. The stack goes up, [the suite runs against
   it](/pipeline/#the-gate), and the commit is tagged `0.0.2` if all of it
   passed. A red run leaves the version published and untagged, so the next push
   rebuilds it, republishes it and runs the suite again.
4. `make update-stack`. There is nothing to edit: the `put-parameter` above
   already wrote the tag to `/asyncdb/version`, and `Version` is an
   `AWS::SSM::Parameter::Value<String>` that reads it, so the update is what
   resolves it. The stack does not pick the new tag up on its own, and even then
   it reaches an instance only when that instance is
   [replaced](/deployment/#rolling-out-a-new-version).

Leaving `version` alone is a deliberate no-op release: the build still runs in
full, and once that version has had one green run the publish steps and the whole
`deploy-and-verify` job skip together, so the push costs the image build and no
AWS at all.
