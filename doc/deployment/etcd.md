# The etcd tier

Three instances running etcd, and a Lambda-backed custom resource that mints the
discovery token they form a cluster with. Five resources, and — as the template
stands — no reader: nothing sets `ASYNCDB_ETCD` on the
[database tier](/deployment/database#the-launch-template), so this cluster comes
up and waits.

## The discovery token

etcd nodes launched by an auto scaling group do not know each other's addresses,
and the template cannot tell them: the addresses do not exist until the
instances do. etcd's answer is a **discovery URL** — a token registered with a
rendezvous service, given to every node, which each node posts its own address
to and reads the others' back from. The first three to arrive are the cluster.

Getting one is an HTTP request, and CloudFormation has no way to make an HTTP
request, so the template carries a Lambda that does:

| Resource | Is |
| --- | --- |
| `DiscoveryTokenLambdaRole` | Assumable by `lambda.amazonaws.com`, with `AWSLambdaBasicExecutionRole` and nothing else — the function touches no AWS API |
| `DiscoveryTokenLambda` | Python 3.9, 30 second timeout, source inline as a `ZipFile` join |
| `DiscoveryTokenCustomResource` | `Custom::EtcdDiscovery`, whose `ServiceToken` is the function's ARN |

The function is nine lines:

```python
import urllib.request
import cfnresponse
def handler(event, context):
    try:
        if event['RequestType'] in ('Create','Update'):
            url = "https://discovery.etcd.io/new?size=3"
            token = urllib.request.urlopen(url).read().decode().strip()
            cfnresponse.send(event, context, cfnresponse.SUCCESS,
                             { 'DiscoveryURL': token })
        else:
            cfnresponse.send(event, context, cfnresponse.SUCCESS, {})
    except Exception as e:
        print("Error:", e)
        cfnresponse.send(event, context, cfnresponse.FAILED, {})
```

`cfnresponse` is not installed — it is a module AWS injects into inline function
source, which is why the code is a `ZipFile` and not an S3 object. The `Delete`
branch answers `SUCCESS` and does nothing, so a token is left registered when
the stack goes; it is a token and it expires, and there is nothing to clean up.

The `Update` branch mints a **new** token on every `update-stack` that reaches
this resource. That does not re-form the running cluster — a token is only read
at first boot — so the effect is a new launch template version whose instances,
when they are eventually replaced, form a *different* cluster from the ones
still running. Discovery is a first-boot mechanism, and this resource treats it
as one only by accident.

Three caveats worth knowing before relying on this:

- **`discovery.etcd.io` is a public service outside the stack**, unauthenticated
  and not run by anyone here. If it does not answer, the function reports
  `FAILED`, the custom resource fails and the stack rolls back — before any
  instance has launched.
- **The protocol is etcd's v2 discovery**, which is deprecated; etcd 3.5
  replaced it with discovery over an existing etcd cluster
  (`--discovery-endpoints`). A stack that has to keep working wants that, or
  wants the addresses fixed some other way.
- **`size=3` is hard-coded** and has to match `EtcdAutoScalingGroup`'s size. A
  token is used up once: the nodes that fill it are the cluster, and a
  replacement instance booting with the same token joins nothing, because the
  token is full and the replacement is not a member.

## The launch template

`EtcdLaunchTemplate` takes the same AMI and instance type, the `asyncdb` key
pair, and `EtcdSecurityGroup`. There is no instance profile, and none is needed:
the image comes from `quay.io` and the function of this instance touches no AWS
API.

```bash
#! /bin/bash
PRIVATE_IP=$(curl -s http://169.254.169.254/latest/meta-data/local-ipv4)
PUBLIC_IP=$(curl -s http://169.254.169.254/latest/meta-data/public-ipv4)
INSTANCE=$(curl -s http://169.254.169.254/latest/meta-data/instance-id)
DISCOVERY_URL='...'                      # Fn::Sub of the custom resource
docker run -d -v /usr/share/ca-certificates/:/etc/ssl/certs -p 4001:4001 -p 2380:2380 -p 2379:2379 \
  --name etcd quay.io/coreos/etcd:v2.3.8 \
  -name ${INSTANCE} \
  -advertise-client-urls http://${PUBLIC_IP}:2379,http://${PUBLIC_IP}:4001 \
  -listen-client-urls http://0.0.0.0:2379,http://0.0.0.0:4001 \
  -initial-advertise-peer-urls http://${PUBLIC_IP}:2380 \
  -listen-peer-urls http://0.0.0.0:2380 \
  -discovery ${DISCOVERY_URL} \
```

The three metadata reads are IMDSv1 — a plain GET to `169.254.169.254` with no
token — and they give the node its name and the addresses it advertises. Only
the `DISCOVERY_URL` line is an `Fn::Sub`; every `${...}` in the rest is a shell
variable, and stays one because the surrounding lines are plain strings in the
`Fn::Join`.

Four things in it are worth changing before it is depended on:

- **It advertises its public addresses.** Peers and clients are told to come in
  over the internet, which is what makes the wide-open 2379–2380 rules on
  [the security group](/deployment/network#the-security-groups) load-bearing
  rather than merely careless. `PRIVATE_IP` is read and never used: advertising
  it instead keeps the traffic inside the VPC and lets both ports be closed to
  everything but the two security groups.
- **The last line ends in a `\` with nothing after it.** A trailing continuation
  at the end of the file is not what was meant, and one more argument appended
  after it would land somewhere unintended.
- **The data directory is not a volume.** etcd writes inside the container, on
  an instance the group may replace, so a replaced node is a new empty node —
  and, because the discovery token is full, not one that can join. Membership is
  small and cheap to lose, but two of three lost at once is a cluster with no
  quorum.
- **It is etcd 2.** asyncdb speaks to etcd over its
  [JSON gateway](/database/cluster#membership) — `POST /v3/kv/put`,
  `/v3/lease/grant` — and v2.3.8 has no v3 API at all. `docker-compose.yml`
  runs `quay.io/coreos/etcd:v3.5.9` for exactly that reason, and this template
  needs to as well before anything can register against it. That is the second
  half of turning the database tier into a cluster; the first half is passing
  the environment.

## The auto scaling group

`EtcdAutoScalingGroup` spans the same three subnets and is pinned:
`MinSize`, `MaxSize` and `DesiredCapacity` are all 3, matching the `size=3` of
the token. `HealthCheckType` is `EC2`, which is right here — there is no target
group to check against, and etcd is not behind the load balancer.

It tags its instances `Name: etcd-node` with `PropagateAtLaunch`, which is what
tells them apart from the database instances in the console. The database group
sets no tags at all.
