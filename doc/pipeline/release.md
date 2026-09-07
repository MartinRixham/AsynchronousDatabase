# Publishing

Publishing is the first three steps of `publish`, after the login and
before the stack: load the image the `build` job left as an artifact, push it to
ECR, and write the tag to `/asyncdb/version`.

**They are in that job because that is where the gate is.** There is one
condition, `needs.build.outputs.verify`, and it is the `if:` on the job — so the
publish is gated by being inside it, exactly as the suite below it is, and no
step repeats the question. The image is republished on every push until the
version passes.

## Why the registry is not the gate

The obvious gate is the registry — is `asyncdb:0.0.2` there already?

```yaml
if aws ecr describe-images \
  --repository-name asyncdb \
  --image-ids imageTag=$VERSION
then
  echo "publish=false" >> $GITHUB_OUTPUT
```

It cannot be the *deploy* gate, because [the stack pulls the image it tests out
of ECR](/deployment/#parameters): the push comes first, in the same job, so a
published version is not yet a version that passed, and skipping the suite for
anything already in the registry would skip it for a version whose first run went
red.

It is a poor **publish** gate too. It would freeze the image at the first commit
that carried the version, so on a red version:

- the fix pushed at the next commit is built by CI and thrown away;
- the stack the suite runs against pulls the **first** commit's image;
- and a green run records a pass for code that was never in the image it tested.

Which is why the answer is [a mutable tag](#overwriting-the-tag) rather than a
second gate.

## Overwriting the tag

`create-repository` sets no `--image-tag-mutability`, so the repository is
`MUTABLE`, which is the default and what this wants.
`asyncdb:0.0.2` is therefore rewritten by every push that carries version `0.0.2`
and has not passed yet, and the last one to be written is the one the suite runs
against and the one the git tag names.

**The window in which a version tag moves is exactly the window before it
passes.** The moment the tag is on the remote, the gate closes and the image is
frozen with it: nothing republishes `0.0.2`, and `docker pull asyncdb:0.0.2` is
the build that went green. What that asks of everybody is one thing — **do not
pull a version that has no git tag**, because it is a version still being worked
on, and neither its bytes nor its number mean anything yet.

## Where the gate is

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

It runs in `build`, and its output is what
[gates the whole `publish` job, and everything downstream of it](/pipeline/#the-gate) — see there for
what it reads and how it fails. The publish is two of that job's steps, and
neither carries a condition of its own:

```yaml
- name: Tag and push Docker image to ECR
  run: |
    IMAGE_URI=${{ steps.ecr-login.outputs.registry }}/asyncdb:$VERSION
    docker load --input image.tar.gz
    docker tag asyncdb:latest $IMAGE_URI
    docker push $IMAGE_URI

- name: Record published version in SSM
  run: |
    aws ssm put-parameter \
      --name /asyncdb/version \
      --type String \
      --value "$VERSION" \
      --overwrite
```

`docker load` is there because the image was built on the *other* job's runner
and [travels as an artifact](/pipeline/#carrying-the-image); the load, the tag
and the push are **one step**. The gate does not notice a push that silently does
nothing: it asks git, not the registry, so such a version is tested as whatever
ECR happens to hold under that tag. If that is nothing, the instances have no
container, the cluster never reaches six nodes and the run fails on the wait —
which is the detection, and it is a coarse one.

**The `put-parameter` is the other half of publishing.** `/asyncdb/version` is
what [the template resolves at deploy time](/deployment/#parameters), so this line
and not the `docker push` is what decides which tag the next instance to launch
will run — including the instances `make create-stack` stands up two steps later,
which is how the suite comes to test what the job above it built.

## Making the repository

Neither ECR repository is a thing anybody creates by hand, and `asyncdb`'s is made
by the step before the push — `describe-repositories` or else `create-repository`,
[as the mirror does for `etcd`](/pipeline/#making-the-repositories). It is what
the `docker push` needs and nothing more.

## Cutting a release

1. Edit `version`. That is the release: nothing else in the repository names it,
   and the number is not read by the build, only by the workflow.
2. Push to `master`. The image builds and the tests inside it run in `build`;
   `publish` then pushes it to ECR and names it in
   `/asyncdb/version`.
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
full, and once that version has had one green run the whole `publish` job is
skipped, and every job that needs it with it, so the push costs the image build
and no AWS at all.
