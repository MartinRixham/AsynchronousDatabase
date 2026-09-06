# The image bake

`.github/workflows/ami.yaml` builds one AMI per tier — the packages a database
instance would otherwise download before it can reach ECR, and the etcd image
three instances have no route to quay.io to pull for themselves. It is **manual**: `workflow_dispatch` and nothing else, because
nothing in it moves when `version` moves.

The workflow is five steps around one command. `ami/asyncdb.pkr.hcl` is the
build, and Packer is what makes the workflow short.

```
              Actions ▸ Build AMIs ▸ Run workflow
                        │  target: both / database / etcd
              ┌─────────┴─────────┐
              │ packer init       │  the amazon plugin, pinned in the template
              │ packer validate   │  and the SSM parameter naming the base image
              └─────────┬─────────┘
              ┌─────────┴─────────┐
              │ packer build -only=<role>.*                          │
              │   launch a builder, from the ECS-optimised AL2023 id │
              │   ami/<role>.sh   as root, over SSH                  │
              │   ami/clean.sh    as root, over SSH                  │
              │   stop, image, terminate, take the key pair and      │
              │   the security group away again                      │
              └─────────┬─────────┘
                        │  manifest.json
            /asyncdb/ami/database   /asyncdb/ami/etcd
```

## What is in them, and what is not

`ami/database.sh` and `ami/etcd.sh` are the provisioning, and each is the first
boot of its tier moved off the boot path:

| | Baked | Still done at boot |
| --- | --- | --- |
| database | `dnf -y update`, `unzip`, AWS CLI v2, `/var/lib/asyncdb` | `docker login`, `docker pull` of `asyncdb:$VERSION`, `docker run` |
| etcd | `dnf -y update`, `quay.io/coreos/etcd:v3.5.9` | `docker run` |

**The asyncdb image is deliberately not baked.** It moves every release, so an
AMI holding it would put an image build on the release path, and the pull it
would save is seconds from a registry in the same region. The user data in
[the launch template](/deployment/database#the-launch-template) still resolves
`/asyncdb/version` and pulls that tag, which is what keeps a release something
an instance picks up when it is replaced rather than something an AMI carries.

The etcd image is the other way round: its tag is pinned in
`cloudformation.json` and does not move with a release, and it was the one third
party either tier reached at start-up. Baking it takes quay.io off the boot path
entirely — and since the tiers moved into
[private subnets reaching AWS services and nothing else](/deployment/network#why-the-instances-are-private),
that is no longer an optimisation: there is no route to quay.io, so **an etcd
AMI that does not hold the tag is an etcd instance with no container.** The
database bake is still only a saving; this half of it is a dependency.

Both scripts `set -euo pipefail` and assert what they installed —
`aws --version | grep '^aws-cli/2\.'`, `docker info` — so a missing dependency is
a failed build rather than an image that is missing half of itself. It matters
because a boot that finds these missing has no container, and the auto scaling
group's health check is `EC2`: nothing would replace the instance for it.

`ami/clean.sh` is the last provisioner of both builds and takes the bake back
out of the image: the ECS agent's state, `cloud-init clean --logs` — cloud-init
records what it has done against the instance id that did it, and an instance
launched from this image is a different one whose own user data has to run — and
the key Packer let itself in with.

## What Packer is doing here

Everything between launching an instance and having an image of it, which is the
part worth not writing: a temporary key pair and security group, the SSH wait, a
provisioner's exit code *being* the build's, the stop, the snapshot, the
terminate, and taking all three temporaries away again — **including when a
provisioner fails**. What is left in the workflow is `init`, `validate`, `build`
and a `put-parameter`.

Two things it needs that the rest of this repository's AWS access does not:

- **Port 22 into the builder.** Packer opens a temporary security group for it,
  by default from `0.0.0.0/0`, which is the `ssh_cidrs` variable and the one
  thing about the builder worth narrowing.
- **A wider IAM user.** On top of running an instance and registering an image:
  `CreateKeyPair` / `DeleteKeyPair`, `CreateSecurityGroup` /
  `AuthorizeSecurityGroupIngress` / `DeleteSecurityGroup`, and
  `ssm:GetParameter` for the base image and `ssm:PutParameter` for the result.

The variables all have defaults in the template, and the workflow passes five:

| Variable | Default | Is |
| --- | --- | --- |
| `region` | `eu-west-2` | An AMI is regional. This has to be the region the stack deploys into |
| `instance_type` | `t3.micro` | The type the stack runs, so what is installed is installed on the hardware it will meet |
| `subnet_id` | *empty* | Empty is the account's **default VPC**, which is not this stack's: the bake needs the public internet and has no part in the network the stack builds |
| `ssh_cidrs` | `0.0.0.0/0` | Who may reach port 22 on the builder |
| `commit` / `run_id` | `unknown` / `local` | Tagged onto the image and the builder — what it was built from, and which run may sweep it up |

The base image is not a variable. `data "amazon-parameterstore"` resolves
`/aws/service/ecs/optimized-ami/amazon-linux-2023/recommended/image_id`, the
public parameter the template resolved for itself until it took these images
instead — so a bake is a layer over whatever the stack would have launched on the
day it ran, and the template no longer resolves anything of Amazon's at all.

## Where the ids go

`/asyncdb/ami/database` and `/asyncdb/ami/etcd`, read out of Packer's
`manifest.json` and written with `put-parameter --overwrite`. This is the pattern
[`/asyncdb/version` already uses](/pipeline/release): the id changes on every
build, so nothing that consumes it should have to be edited when it does.

`cloudformation.json` reads them as its `DatabaseAmi` and `EtcdAmi`
[parameters](/deployment/#parameters), both
`AWS::SSM::Parameter::Value<AWS::EC2::Image::Id>`, so a deploy resolves whatever
the last bake wrote rather than what the working tree says. **They have to exist
before the deploy that reads them**, exactly as `/asyncdb/version` does:
CloudFormation cannot resolve a parameter that is not there and fails the whole
stack operation rather than the one resource.

A bake does not reach a running instance. The auto scaling group takes the new
launch template version, and an instance launched after that carries the new
image — so, like a release, an AMI reaches the fleet one instance replacement at
a time.

## When to run it

- To take a newer Amazon Linux, which then happens because somebody said so and
  not because an instance was replaced on a Tuesday.
- When the etcd tag in `cloudformation.json` changes. The tag is written twice —
  there and in `ami/etcd.sh` — and nothing makes them agree.
- When the database tier's boot dependencies change.

Not on a release. `version` changing has nothing to do with these images.

## Running it by hand

The workflow is the only thing that writes the SSM parameters, but the build
itself is a local command, and the same one:

```bash
packer init ami/asyncdb.pkr.hcl
packer build -only='database.*' -var commit=$(git rev-parse HEAD) ami/asyncdb.pkr.hcl
```

It needs credentials that can do the list above, and a few minutes, most of which
is the snapshot.

## What this does not do

- **Nothing prunes them**, or the snapshot behind each one. Every run registers
  an image and deregisters none, the way ECR accumulates tags.
- **Nothing checks that an image is current.** A parameter pointing at a bake
  from a year ago deploys happily, with a year-old Amazon Linux on it — which is
  the deliberate half of the trade the
  [parameters](/deployment/#parameters) make: no drift, and no refresh either. The
  `asyncdb:commit` and `asyncdb:source-image` tags are the only record of what a
  given AMI was built from.
- **A cancelled run is the one ending Packer may not be given time to clean up
  after**, which is what the workflow's last step is: the builder carries the
  run's id as a tag, so a run sweeps up its own instance and never another one's.
- **`packer fmt` disagrees with this file.** It wants two spaces and aligned
  `=`; the repository is tabs throughout, and nothing runs `packer fmt`.
- An AMI is regional and account-local, one more thing tying this stack to one
  `eu-west-2`.
