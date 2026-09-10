# Membership and leadership

etcd holds two kinds of key for asyncdb and nothing else:

| Key | Is | On |
| --- | --- | --- |
| `/asyncdb/node/{address}` | `{"node":...,"zone":...}` — a member | A ten second lease |
| `/asyncdb/leader/{partition}` | The address of the node leading that partition | The same node's lease |

Both are leased, so **neither outlives the node that wrote it**, and neither is
worth preserving: a cluster whose etcd came back empty repopulates itself within
a lease. That is what makes most of what goes wrong here fix itself.

Every node renews its lease every **three seconds** — a third of the lease, so
two chances to be renewed before it runs out — and on the same pass reads the
membership back and claims up to **64** of the 256 partitions.

## Reading it directly

etcd is spoken to over its JSON gateway, so `curl` is enough and no `etcdctl` is
needed. Keys and values are base64 in both directions, and a prefix range ends
at the prefix with its last byte raised — `/asyncdb/node/` to `/asyncdb/node0`:

```bash
ETCD=http://$(aws ec2 describe-instances --filters Name=tag:Name,Values=etcd \
  Name=instance-state-name,Values=running \
  --query 'Reservations[0].Instances[0].PrivateIpAddress' --output text):2379

# the membership
curl -s $ETCD/v3/kv/range -d "{
  \"key\": \"$(printf '/asyncdb/node/' | base64 -w0)\",
  \"range_end\": \"$(printf '/asyncdb/node0' | base64 -w0)\"
}" | jq -r '.kvs[]? | (.key | @base64d) + " -> " + (.value | @base64d)'

# who leads what
curl -s $ETCD/v3/kv/range -d "{
  \"key\": \"$(printf '/asyncdb/leader/' | base64 -w0)\",
  \"range_end\": \"$(printf '/asyncdb/leader0' | base64 -w0)\"
}" | jq -r '.kvs[]? | (.key | @base64d) + " -> " + (.value | @base64d)'
```

Counting the second one is the quickest answer to "has the election settled":
256 keys is every partition led.

## etcd cannot be reached

**This is the failure mode worth understanding before it happens**, because what
a node does about it is deliberate, defensible, and surprising.

A node that cannot reach any member of etcd **carries on as a cluster of one**.
It reads no membership, so it puts itself in the list, and a membership of one
means:

- it holds **every** key, so it answers every read out of its own store —
  including keys another node holds, for which it will answer `404`;
- there is nothing to order, so it accepts **every** write locally, with no
  leader and no copies;
- it asks nobody for a scan, so a scan answers only what it holds;
- `/health` names **one** node, itself, in a `zones` of one zone, which is how
  this is recognised. Not *no* `nodes`: an absent `nodes` is
  [an instance that stands alone](/database/cluster#what-each-endpoint-does-in-a-cluster),
  one that was never given an `ASYNCDB_ETCD` to lose. A node that has lost etcd
  puts itself in the list, and a list of one is what that looks like.

That is the safe way to be wrong on the reasoning that refusing to answer would
turn one broken etcd into a broken database — but it is only safe while the node
is *also* unreachable by clients. **A node that has lost etcd but is still taking
client traffic is the one situation here that can silently diverge the data**,
because the writes it accepts are written nowhere else and no leader ordered
them.

**And nothing takes it out of service for you.** The load balancer's health check
is `/asyncdb/health`, which answers `200` whatever the membership says — `status`
is always `ok`, and a cluster of one is a node that is serving perfectly well by
that measure. So an isolated node stays in the target group, goes on being routed
to, and answers `404` for every key it does not hold. Taking it out is step 2
below and it is a hand's work, not the load balancer's.

**Do:**

1. `/health` on every node. A node whose `nodes` names **only itself** has lost
   etcd; the ones that still name the others have not. It goes on reporting a
   non-zero `leads` — the partitions it last claimed — so `leads` does not fall
   to zero to tell you, and the length of `nodes` is the whole diagnosis.
2. Take that node out of service if it is behind the load balancer, or stop it.
   Its keys are held in every other zone and reads carry on without it.
3. Fix etcd — see below — and let the node re-register. It re-registers from
   scratch on the pass after a renewal fails rather than believing it is still a
   member, so nothing needs restarting.
4. Rewrite anything written to it while it was isolated. Nothing reconciles it.

Naming every etcd member in `ASYNCDB_ETCD` is what makes this rare: any member
answers for the whole cluster, a member that does not answer is a reason to try
the next, and the node stays with whichever answered. Naming one member makes
that member a single point of failure for the membership.

```
ASYNCDB_ETCD=http://10.0.0.37:2379,http://10.0.1.204:2379,http://10.0.2.19:2379
```

The addresses are whatever the etcd instances had **when this node booted** —
[`DescribeInstances` reads them once](/deployment/etcd#how-the-database-tier-finds-it)
— so an entry that answers nothing is an etcd instance that has since been
replaced, and two of the three answering is the tier working as intended.

A member that *refuses* a request rather than failing to answer is not a reason
to try the next one: it has given the answer the whole cluster would give.

## etcd has lost quorum

Two of three members gone. The survivor answers reads of already-committed keys
but takes no writes, so leases stop being renewed, membership keys expire, and
every node falls back to the cluster-of-one behaviour above within ten seconds.

**This is the one etcd failure [the group](/deployment/etcd#the-group) does not
heal**, and it fails loudly rather than quietly. The replacements it launches ask
the survivor to admit them, `member add` needs a quorum to agree and there is
none, and they **exit without starting etcd** rather than bootstrap a cluster of
their own — which would leave the survivor running a second one and the database
tier holding endpoints for both. An instance with no container is the symptom.

**Do:** replace the whole tier, which is the right answer only because the data
is a few leased keys that rewrite themselves in seconds:

```bash
aws ec2 describe-instances --filters Name=tag:Name,Values=etcd \
  Name=instance-state-name,Values=running \
  --query 'Reservations[].Instances[].InstanceId' --output text \
  | xargs aws ec2 terminate-instances --instance-ids
```

The group launches three more, none of them finds a cluster to join, and they
bootstrap one from the instances `DescribeInstances` shows them. It costs one
lease of stale membership.

**Then roll the database tier**, and this is the part that is easy to forget:
`ASYNCDB_ETCD` is
[read once at boot](/deployment/etcd#how-the-database-tier-finds-it), so after
all three etcd instances have been replaced, every running asyncdb node holds
three addresses that answer nothing and is
[a cluster of one](#etcd-cannot-be-reached). One database instance at a time,
letting each come back before the next goes.

## A replacement etcd instance did not join

The group launched one and there is no container on it, or `member list` still
names a member that no instance answers for. The
[boot script](/deployment/etcd#the-launch-template) writes what it did to
cloud-init's log, and that is the first thing to read:

```bash
aws ssm start-session --target i-0123456789abcdef0
sudo tail -50 /var/log/cloud-init-output.log
```

| In the log | Is |
| --- | --- |
| `member add` refused, and the script exited | No quorum. The section above is the recovery |
| `UnauthorizedOperation` on `DescribeInstances` | The instance profile, or [the route out](/deployment/network#the-route-out) the call goes over |
| The cluster string names fewer than three | The other instances were not visible within five minutes — check they carry `Name=etcd` |
| Nothing at all after the docker run | etcd started and exited. `docker logs etcd` says why; a cluster ID mismatch means the data directory was not empty |

**The member dance is still there to do by hand**, on a member that answers:

```bash
etcdctl member list
etcdctl member remove <the id with no instance>
etcdctl member add etcd-i-0abc… --peer-urls=http://10.0.1.37:2380
```

then start the new node with `--initial-cluster-state existing` and the member
list the add printed. It is the same three commands the boot script runs; doing
them by hand is for when it did not get to.

## The membership is wrong

`/health` on two nodes names different sets. Read it as a moment rather than a
state: a node registers before it starts serving, and every node reads the list
again every three seconds, so a difference that is a few seconds old is normal.

**A difference that persists** is one of:

| Seen | Is |
| --- | --- |
| A node missing from every list but itself | That node cannot reach etcd, or has not started. Its own `/health` says which |
| A node missing from one list only | That reader cannot reach etcd. Its own `nodes` will name only itself |
| A node named that is gone | Within ten seconds, its lease running out. Longer, and something is still renewing it |
| Fewer `zones` than the deployment has | A whole zone's nodes are gone, or `ASYNCDB_ZONE` is unset on them |
| A node named in no zone | It registered a bare address, or with `ASYNCDB_ZONE` empty |

The last one matters more than it looks: **a node in no zone is its own zone**
as far as grouping goes, so a cluster where one node lost its `ASYNCDB_ZONE`
keeps an extra copy in a zone that does not exist, and scans of that zone answer
from one node. Check the environment of the container:

```bash
docker inspect asyncdb-2 --format '{{range .Config.Env}}{{println .}}{{end}}' | grep ASYNCDB
```

On the AWS stack `ASYNCDB_ZONE` is the instance's real availability zone, read
out of IMDS in the user data, so a node in no zone there is a metadata read that
failed at boot. Replacing the instance is the fix, and it is
[an empty database](/runbook/storage#a-node-came-back-empty) again.

## No partition has a leader

**Looks like:** every write answers `503 no_leader`; reads are perfectly fine;
`leads` is `0` on every node.

Reads never wait for a leader, which is why this presents as a write-only outage.

| `leads` across the cluster | Means |
| --- | --- |
| Sums to 256 | Settled. Every partition is led |
| Sums to less, and rising | A cold cluster still claiming. 64 per node per pass — wait a pass or two |
| `0` everywhere, not rising | No node can write to etcd. Claims are transactions, so a read-only etcd claims nothing |
| Sums to 256 but a write still says `no_leader` | The leader of that partition is a node this one cannot reach, or the two disagree about the membership |
| Sums to 256, and a node that just joined leads none of it | The nodes the membership stopped naming have not given those claims up yet. Two passes, so seconds — longer, and they cannot write to etcd |
| Sums to 256, and one node leads far more of it than the others | The membership those nodes read does not agree. `leads` is worked out from the same hashing on every node, so an even split is what agreement looks like |

A node claims only partitions the membership names it to lead, on its own lease,
and only while it *has* a lease — a node with no lease has nothing to claim on
and would otherwise leave a leadership behind that nothing ever expires. So
`leads: 0` everywhere is nearly always etcd, and the fix is
[etcd cannot be reached](#etcd-cannot-be-reached).

It gives up what the membership stops naming it for, too, which is what moves
leadership after a resize: nothing but a lease takes a claim away, so a node that
kept its lease through a membership change would otherwise lead the same
partitions for as long as it ran, and a node that joined would lead nothing at
all.

**Do not try to force an election.** There is no way to, and none is needed: the
node that should lead a partition is worked out from the membership, so a
partition nothing leads is claimed by that node on its next pass.

## Leadership keeps moving

**Looks like:** `409 stale_leader` steadily rather than once; `leads` swinging
between nodes on repeated `/health` calls.

A leader stops leading when it stops renewing its lease, so leadership moving
constantly is a node whose renewals keep failing — a node that is slow, or whose
path to etcd is unreliable. The renewal is every three seconds against a ten
second lease and the etcd client waits five seconds, so a node needs two
consecutive slow answers to be dropped.

**Check** the node's logs for `could not register with etcd`, and the latency
from that node to each etcd member:

```bash
for e in $(aws ec2 describe-instances --filters Name=tag:Name,Values=etcd \
    Name=instance-state-name,Values=running \
    --query 'Reservations[].Instances[].PrivateIpAddress' --output text); do
  curl -s -o /dev/null -w "$e %{time_total}\n" --max-time 5 \
    http://$e:2379/v3/kv/range -d '{"key":"AA=="}'
done
```

**Do:** take the flapping node out. Its partitions are led by another copy within
a lease, and the `stale_leader` answers stop.

## What a term does not survive

**A term is remembered in memory, not on disk.** A node that restarts has
forgotten which terms it has applied, so it accepts the first write it is sent
afterwards whatever term ordered it. A node that restarts on AWS has also lost
its RocksDB, so this is a smaller hole than it sounds — but it is a hole, and it
is the reason a `stale_leader` that appears immediately after a restart is not
evidence that the fencing failed.

## What this cannot tell you

- **Which copy is right.** There is no version, no vector clock and nothing to
  compare — a leader orders writes, it does not replicate them. If two copies
  disagree, the remedy is to write the record again and make both of them right.
- **Whether a write reached every copy.** The client is told a write failed; it
  is not told which copies took it. That is why every write is idempotent and
  why the remedy is always to run the whole request again.
- **Anything about a node that is not registered.** A node that never reached
  etcd is invisible to the others, and they will happily own the keys it is
  serving to whoever can still reach it.
