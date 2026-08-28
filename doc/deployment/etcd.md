# The etcd tier

Three instances at three addresses the template already knows. There is no
discovery service, no Lambda, no custom resource and no auto scaling group:
`Etcd1`, `Etcd2` and `Etcd3` are three `AWS::EC2::Instance` resources with fixed
private addresses, and the membership they bootstrap from is a literal string in
`Mappings`.

## Why there is nothing to discover

etcd holds one thing for asyncdb: a key per node,
[`/asyncdb/node/{address}` on a ten second lease](/database/cluster#membership).
Every asyncdb node renews its own every three seconds, and re-registers from
scratch whenever a renewal fails, so an etcd cluster that comes back empty is
fully repopulated within a lease. **There is no state here worth preserving, and
no node whose identity has to survive.**

That is what makes the discovery question small. A mechanism for nodes that must
find each other without knowing where they are is solving a problem this stack
does not have: it is three instances, in three subnets whose CIDRs the template
fixes, and the addresses can be fixed too.

The stack used to mint a token from `discovery.etcd.io` in a Lambda-backed
custom resource and pass it to an auto scaling group. That is gone, and it is
worth recording why, because the shape is a common one:

- **The service behind it is unmaintained.** It runs on the v2 storage engine
  that v3 replaced, and etcd 3.6 removes v2 discovery outright. A stack whose
  creation depends on a third party's deprecated endpoint answering is a stack
  that rolls back the day it stops.
- **There was no version to upgrade to.** v3 discovery — `--discovery-token`
  with `--discovery-endpoints` — bootstraps from *another* etcd cluster, which
  is no use to the only etcd cluster in the stack.
- **A token is used up once.** `size=3` filled by the first three to arrive, so
  a replacement instance booting with the same token joined nothing. The auto
  scaling group that was supposed to make the tier self-healing could not heal
  it.
- **An update minted a new one.** Every `update-stack` reaching the custom
  resource produced a fresh token, so the launch template's next instances would
  have formed a *different* cluster from the ones still running.

## The addresses

```json
"Mappings": {
  "Etcd": {
    "Cluster": {
      "InitialCluster": "etcd-1=http://10.0.0.10:2380,etcd-2=http://10.0.1.10:2380,etcd-3=http://10.0.2.10:2380",
      "ClientEndpoints": "http://10.0.0.10:2379,http://10.0.1.10:2379,http://10.0.2.10:2379"
    }
  }
}
```

| Node | Subnet | CIDR | Address |
| --- | --- | --- | --- |
| `etcd-1` | `PublicSubnet1` | `10.0.0.0/24` | `10.0.0.10` |
| `etcd-2` | `PublicSubnet2` | `10.0.1.0/24` | `10.0.1.10` |
| `etcd-3` | `PublicSubnet3` | `10.0.2.0/24` | `10.0.2.10` |

`.10` is arbitrary but not free: AWS reserves the first four addresses of a
subnet and the last, so anything from `.4` up will do.

The two strings are the whole of the wiring, and each is read once:

- **`InitialCluster`** is `--initial-cluster`, the same on all three nodes.
- **`ClientEndpoints`** is `ASYNCDB_ETCD` on the
  [database tier](/deployment/database#the-launch-template), which takes
  [every member separated by commas](/database/cluster#turning-it-on) and stays
  with whichever answered. That is why there is no load balancer and no DNS in
  front of etcd: naming all three *is* the failover.

## The instances

Each one is fifteen lines of properties and a user data script, differing only
in its number, its subnet and its address:

```json
"Etcd1": {
  "Type": "AWS::EC2::Instance",
  "DependsOn": [ "PublicRoute", "SubnetRouteTableAssociation1" ],
  "Properties": {
    "ImageId": { "Ref": "ECSAMI" },
    "InstanceType": { "Ref": "InstanceType" },
    "SubnetId": { "Ref": "PublicSubnet1" },
    "PrivateIpAddress": "10.0.0.10",
    "SecurityGroupIds": [ { "Ref": "EtcdSecurityGroup" } ],
    "KeyName": "asyncdb",
    "Tags": [ { "Key": "Name", "Value": "etcd-1" } ],
    "UserData": { "...": "below" }
  }
}
```

The `DependsOn` is there because the user data pulls an image at first boot: an
instance created before its subnet has a route to the internet gateway comes up
with nothing on it and says nothing about why. An auto scaling group hid that by
launching late; three instances do not.

```bash
#! /bin/bash
docker run -d --restart always --name etcd -p 2379:2379 -p 2380:2380 \
  -v /var/lib/etcd:/etcd-data \
  quay.io/coreos/etcd:v3.5.9 /usr/local/bin/etcd \
  --name etcd-1 \
  --data-dir /etcd-data \
  --advertise-client-urls http://10.0.0.10:2379 \
  --listen-client-urls http://0.0.0.0:2379 \
  --initial-advertise-peer-urls http://10.0.0.10:2380 \
  --listen-peer-urls http://0.0.0.0:2380 \
  --initial-cluster etcd-1=http://10.0.0.10:2380,... \
  --initial-cluster-state new \
  --initial-cluster-token asyncdb
```

The `--initial-cluster` line is a `Fn::Join` of the literal and the mapping; the
rest are plain strings. Four things about it:

- **It advertises private addresses.** Peers and clients are told to come in over
  the VPC, which is what lets both ports be closed to everything but the two
  security groups — see [the network](/deployment/network#the-security-groups).
  The instances still have public addresses, because they are in public subnets
  and have to reach `quay.io`, but nothing is invited in over them.
- **`/usr/local/bin/etcd` is named explicitly**, as `docker-compose.yml` names
  it: the image has no entrypoint of its own, and the flags are the v3 ones
  (`--name`, not `-name`) on the v3.5.9 that serves the
  [JSON gateway](/database/cluster#membership) asyncdb speaks.
- **`--restart always`** matters more than it did: nothing replaces these
  instances now, so a container that exits has to come back by itself. `-v
  /var/lib/etcd` puts the data outside it so that it does.
- **`--initial-cluster-state new` applies only to an empty data directory.** A
  reboot, or a container restart, finds the data directory and rejoins as the
  member it was; `new` is read at bootstrap and ignored afterwards. The
  `--initial-cluster-token` is what keeps this cluster's members from ever
  joining another one by accident.

## What a failure looks like

An availability zone takes one node. Two remain, they have quorum, and the
database tier does not notice: it names all three endpoints, finds the dead one
unreachable and stays with one that answers.

Getting the third one back is where fixed addresses cost something. `make
update-stack` recreates the instance at the same address, but with an empty data
directory and a new member id, and the two survivors still hold the old id for
`etcd-1`. It will not simply rejoin. There are two ways out:

- **Replace the tier.** The data is a few keys that rewrite themselves in
  seconds, so deleting all three instances and letting `update-stack` recreate
  them is a clean bootstrap and costs one lease of stale membership. This is
  usually the right answer, and it is only the right answer *because* the data
  is disposable.
- **Do the member dance.** `etcdctl member remove <old id>` on a survivor,
  `etcdctl member add etcd-1 --peer-urls=http://10.0.0.10:2380`, then start the
  new node with `--initial-cluster-state existing` and the member list the add
  printed. This is what an auto scaling group would have to automate, and the
  reason it is not worth automating here.

Losing two at once is a cluster with no quorum, and the remedy is the first one.

## What is left to it

- **Nothing replaces a dead instance.** That is the trade for determinism: an
  auto scaling group heals an instance and cannot heal a *member*, which is the
  thing that was actually broken.
- **There is no TLS and no authentication.** 2379 is open to the database tier's
  security group and 2380 to etcd's own, so anything on the database tier can
  read and write the membership — including writing in a node that does not
  exist, which the other nodes would then forward keys to.
- **Three nodes is the minimum that tolerates one loss**, and the addresses,
  the mapping and the `--initial-cluster` string all have to be edited together
  to change that. Growing to five is a template change, not a capacity change.
