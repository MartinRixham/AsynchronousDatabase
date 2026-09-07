# When something is wrong

Everything in asyncdb that can fail, what it looks like from the outside, and
what to do about it. The pages under this one are organised by *where* the fault
is — the client's view, a node, the membership, the store, the deployment — and
every one of them says what recovers by itself and what needs a hand, because
most of what goes wrong here recovers by itself.

The system is small enough to hold in the head, and three sentences cover most
of an incident:

- **A read needs one copy.** Any zone can be gone and a record is still read: a
  copy that does not answer is passed over for the next.
- **A write needs a leader and every copy.** A partition with no leader, or a
  zone that is down, is a write that is refused — and refused rather than lost,
  because every write here is idempotent and the remedy is to run it again.
- **Nothing repairs anything.** There is no read repair, no replication log, no
  rebalancing and no backup. A copy that missed a write stays missing until the
  record is written again, and a node that is replaced comes back empty.

That last one is the important one. Most of the recovery steps on these pages
end in *write the data again*, because there is nothing else that can put it
back. Where that is the answer, the page says so plainly rather than implying a
repair that does not exist.

## The first thing to do

`GET /asyncdb/health` on the address that is misbehaving, and then on each node
in turn. It is the only diagnostic the server has, and it answers four questions
at once:

```bash
curl -s http://localhost:8080/asyncdb/health | jq
```

```json
{
  "status": "ok",
  "write_stalled": false,
  "nodes": [ "http://asyncdb-1:8080", "http://asyncdb-2:8080", "http://asyncdb-3:8080" ],
  "zones": {
    "one": [ "http://asyncdb-1:8080", "http://asyncdb-2:8080" ],
    "two": [ "http://asyncdb-3:8080" ]
  },
  "leads": 128
}
```

| Field | Read it as |
| --- | --- |
| no answer at all | The process is not serving, or nginx is answering for it — [a node that has failed](/runbook/nodes) |
| `status` | Always `ok`. It says the process is answering, and nothing more |
| `write_stalled` | `true` is [RocksDB applying back pressure](/runbook/storage#writes-are-stalled) on this node |
| `nodes` | The membership **as this node sees it**. A list naming only this node is [a node that has lost etcd](/runbook/membership#etcd-cannot-be-reached); absent entirely is an instance that was never clustered at all |
| `zones` | The grouping, and so the number of copies. The number of zones *is* the number of copies |
| `leads` | How many of the 256 partitions this node orders the writes of. `0` on every node is [an election that has not settled](/runbook/membership#no-partition-has-a-leader) |

**Ask every node, not one.** The answer is that node's own view, and the
disagreements between them are the diagnosis: a node missing from one node's
`nodes` and present in another's is a membership that has not settled, and a
`zones` with fewer zones than the deployment has is a whole copy of the keyspace
that is not there.

```bash
for port in 8080 8081 8082; do
  echo "== $port"
  curl -s --max-time 5 "http://localhost:$port/asyncdb/health" | jq -c '{nodes, zones, leads, write_stalled}'
done
```

Against the AWS stack there is one address and six nodes behind it, so the load
balancer picks a different one each time and repeating the call is how each of
them is reached — see [the deployment](/runbook/deployment#asking-one-instance).

## Triage

| What is seen | Likely | Page |
| --- | --- | --- |
| `502` or `504`, HTML body | The database is not answering the nginx in front of it | [A node has failed](/runbook/nodes#the-container-has-stopped) |
| `503 no_leader` on writes, reads fine | A leader is being claimed, or etcd cannot be reached | [Leadership](/runbook/membership#no-partition-has-a-leader) |
| `503 write_stalled` | RocksDB back pressure on the node that took the write | [The store](/runbook/storage#writes-are-stalled) |
| `409 stale_leader` | A leader was replaced mid-write | [What a client sees](/runbook/errors#_409-stale-leader) |
| `500 storage_error`, "did not answer" | A node in the fan-out is unreachable | [A node has failed](/runbook/nodes#a-node-does-not-answer) |
| `500 storage_error`, "failed:" | RocksDB itself returned an error | [The store](/runbook/storage#rocksdb-returned-an-error) |
| `404 table_not_found` for a table that exists | A node came back empty and the tables were not redeclared | [The store](/runbook/storage#a-node-came-back-empty) |
| `404` for a record that was written | A copy missed the write, or the table was dropped | [What a client sees](/runbook/errors#a-record-that-should-be-there-is-not) |
| `400 invalid_cursor` mid-scan | The page was asked of a different node | [What a client sees](/runbook/errors#_400-invalid-cursor-part-way-through-a-scan) |
| Scans fail, reads and writes fine | Every zone has a node that does not answer | [A node has failed](/runbook/nodes#a-scan-fails-while-everything-else-works) |
| `health` names too few nodes | Membership has not settled, or etcd is unreachable | [Membership](/runbook/membership#the-membership-is-wrong) |
| Instances replaced over and over | The image cannot be pulled, or the grace period is too short | [The deployment](/runbook/deployment#instances-are-replaced-in-a-loop) |
| Everything gone after a deploy | An instance was replaced, which is an empty database | [The store](/runbook/storage#a-node-came-back-empty) |
| A node is in the cluster but holds nothing | It was replaced, and its rebuild found no zone to read | [Rebuilding a node](/runbook/rebuild) |

## What recovers by itself

Worth knowing before reaching for anything, because acting during one of these
windows usually makes it longer:

| Condition | Clears in | By |
| --- | --- | --- |
| A node's membership after it dies | up to 10 seconds | Its etcd lease running out |
| A partition whose leader died | up to 10 seconds, then a pass | Another copy claiming it |
| A cold cluster leading nothing | a pass or two, seconds | 64 claims per node per pass |
| A node that could not reach etcd | the next pass, ~3 seconds | Re-registering from scratch |
| A container that exited | at once | `docker run --restart always` |
| A replaced node's records | its start-up | [It rebuilds from another zone before joining](/runbook/rebuild) |
| RocksDB back pressure | seconds to minutes | Compaction catching up |

And what does not, ever, without a hand: **a copy that missed a write on a node
that is otherwise whole**, **anything at all when there is only one zone**, and
**an etcd member recreated at an address the survivors already know**. The
rebuild covers an *empty* node, not a thin one.

## The pages

| Page | Covers |
| --- | --- |
| [What a client sees](/runbook/errors) | Every error code, what causes it, and whether to retry |
| [A node has failed](/runbook/nodes) | A process, container or instance that is gone, slow, or wrong |
| [Membership and leadership](/runbook/membership) | etcd, the node list, leases, terms and elections |
| [The store](/runbook/storage) | RocksDB: stalls, errors, disks, and data that is not there |
| [The deployment](/runbook/deployment) | The stack, the image, the load balancer and the release |
| [Rebuilding a node](/runbook/rebuild) | How a replaced node fills itself in before it joins, and when it does not |

## What checks that this page is still true

`chaos/` is this runbook as a test suite. Each experiment injects one of the
failure modes above into the deployed stack with the AWS CLI — an instance
stopped, an availability zone cut off, etcd's quorum taken away — and asserts
that what happens is what the page for it says happens, and that what recovers
by itself does. The build runs it after the load tests, so a page here that has
gone out of date fails a release rather than an incident.

Not everything on these pages is a fault worth injecting from outside.
`chaos/README.md` lists what it covers and what it deliberately does not.
