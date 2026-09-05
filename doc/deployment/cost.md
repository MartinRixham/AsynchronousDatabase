# What it costs

The stack is six `t3.micro` instances, one load balancer and six thirty gigabyte
volumes, and **none of it is billed per request**. There is no API gateway
metering calls, no per-operation storage service, no serverless anything: the
bill is a fixed monthly cost for capacity that is running whether or not anybody
is using it, plus four marginal charges that a request can move.

So the answer to "what does a read cost" is not a price. It is:

```
cost of a request = fixed monthly cost / requests served that month
                  + bytes it sends to the internet
                  + bytes it sends across an availability zone
                  + the load balancer capacity it uses
                  + the CPU credits it burns
```

The first term is the one that dominates, and it falls as the stack is used
harder. The last term is the one that surprises people, and it is dealt with
[below](#cpu-credits).

**If you want the numbers rather than the model, they are in
[reads, writes and gigabytes](#reads-writes-and-gigabytes)** — per million
reads, per million writes and per gigabyte-month, at a range of record sizes and
throughputs. The short version, at 1 KiB records and 100 requests a second:

| | Per million |
| --- | --- |
| Reads | **$0.559** |
| Writes | **$0.508** |
| Storage | **$1.26 / GB-month** of records at a realistic fill |

Of which $0.459 is the rent, which every operation pays equally and which is
[the only term that moves much](#putting-it-together).

## The prices

::: warning These are list prices, and they go stale.
Everything below is USD on-demand list price in **eu-west-2**, which is
[the only region this stack deploys into](/deployment/#the-address), read at
the time of writing. Check them against
[the AWS pricing pages](https://aws.amazon.com/ec2/pricing/on-demand/) before
quoting a number to anyone. The arithmetic is the part worth keeping; the
inputs are not.
:::

| Thing | Price | Notes |
| --- | --- | --- |
| `t3.micro`, Linux, on demand | $0.0118 / hour | Both tiers use it, from the single `InstanceType` [parameter](/deployment/#parameters) |
| T3 surplus CPU credits | $0.05 / vCPU-hour | Only above baseline, and only in `unlimited` mode — see [below](#cpu-credits) |
| EBS gp3 storage | $0.0928 / GB-month | 3,000 IOPS and 125 MB/s are [included, not extra](/deployment/database#the-root-volume) |
| Application load balancer | $0.0265 / hour | Plus capacity units |
| Load balancer capacity unit | $0.008 / LCU-hour | The max of four dimensions, [not their sum](#load-balancer-capacity-units) |
| Public IPv4 address, in use | $0.005 / hour | Per address, charged since February 2024 |
| Data out to the internet | $0.09 / GB | After 100 GB a month, free across the account |
| Data between availability zones | $0.01 / GB | Charged **in each direction**, so $0.02 per GB that crosses |
| Data in from the internet | free | Which is why [a write is cheaper than a read](#what-each-endpoint-costs) |
| ECR storage | $0.10 / GB-month | One image, a few hundred megabytes |
| SSM standard parameters | free | `/asyncdb/version` and the AMI lookup |

Assume a 730 hour month throughout.

## Standing still

Nothing has to be running for this to be charged. It is the cost of the stack
existing.

| Line | Arithmetic | Monthly | Share |
| --- | --- | --- | --- |
| Database instances | 3 × $0.0118 × 730 | $25.84 | 21% |
| etcd instances | 3 × $0.0118 × 730 | $25.84 | 21% |
| Root volumes | 6 × 30 GB × $0.0928 | $16.70 | 14% |
| Load balancer hours | $0.0265 × 730 | $19.34 | 16% |
| Public IPv4 addresses | 9 × $0.005 × 730 | $32.85 | 27% |
| | | **$120.58** | |

That is **$0.165 an hour**, or about $1,450 a year, for an empty database. Three
things in it are worth saying out loud:

- **The public addresses cost more than the load balancer.** There are nine of
  them: one on each of the six instances, because
  [every subnet is public](/deployment/network#why-everything-is-public) and
  `MapPublicIpOnLaunch` is true, and one per subnet on the load balancer itself.
  They are 27% of the bill and they exist so that instances can reach ECR and
  `quay.io` at first boot. Private subnets would remove six of them and add a
  NAT gateway that costs more than all nine together, which is exactly the
  trade the network page already describes — the cheap answer is still the cheap
  answer, it is just no longer free.
- **The etcd tier is a fifth of the bill**, and it holds
  [three keys on a ten second lease](/database/cluster#membership). It is
  membership only. Nothing about it needs a `t3.micro` each, and nothing about
  it needs three separate instances except the wish to survive an availability
  zone.
- **The volumes are paid for empty.** EBS bills provisioned capacity, so
  180 GB is charged whether the database holds a byte or thirty gigabytes.
  [Storage has no marginal price here](#storage) at all.

### What that is per request

Divide the fixed cost by the requests served, and the shape of the whole thing
appears:

| Sustained load | Requests a month | Fixed cost per million |
| --- | --- | --- |
| 1 / second | 2.6 M | $45.88 |
| 10 / second | 26 M | $4.59 |
| 100 / second | 263 M | $0.46 |
| 250 / second | 657 M | $0.18 |

**A request has no price of its own; it has a share of the rent.** Everything
below this line is small next to moving up that table, so the first question
about the cost of an API is not what an operation costs but how busy the stack
is — and the second is where it stops going, which is [capacity](#the-ceiling).

## Reads, writes and gigabytes

The unit prices, for anyone who wants a number to multiply. They assume the
[reference workload](#the-reference-workload) below; the terms they are built
from are derived in [what a request adds](#what-a-request-adds).

### The reference workload

| Assumption | Value | If yours differs |
| --- | --- | --- |
| Record size | 1 KiB | [Scale the marginal column](#by-record-size) — it is linear in bytes |
| Connections | keep-alive | Without it, add ~$0.09 per million requests of [LCU](#load-balancer-capacity-units) |
| CPU | within baseline | Above it, add [$0.0139 per million per millisecond](#the-cpu-term) of CPU |
| Cluster | 3 nodes, 1 per AZ, [one copy per zone](/database/cluster#one-copy-in-every-zone) | Every node holds every key, so a read is local and a write is copied to the other two |
| Egress allowance | already spent | The first 100 GB a month is free, worth $9 |

### What a read and a write cost

Two terms: a share of the [fixed cost](#standing-still), which depends only on
how busy the stack is, and the bytes, which depend only on the size of the
record.

| Sustained load | Rent share | **Read of 1 KiB** | **Write of 1 KiB** |
| --- | --- | --- | --- |
| 1 / second | $45.88 | **$45.980** | **$45.929** |
| 10 / second | $4.59 | **$4.690** | **$4.639** |
| 100 / second | $0.459 | **$0.559** | **$0.508** |
| 250 / second | $0.184 | **$0.284** | **$0.233** |

All per million operations. The marginal part of that — the part that is
genuinely the operation's own — is **$0.100 per million reads** and **$0.049 per
million writes**, and it is the same at every row. Everything else in the table
is the rent moving.

**A read costs 2.0× what a write costs**, per byte. A read's bytes go out to the
internet at $0.09/GB and cross no zone, because the node the load balancer
picked holds a copy of the key; a write's bytes arrive for free and then cross
into each of the other two zones at $0.02/GB apiece. Keeping a copy in every
zone is what moved that charge from the read to the write — the cheaper end of
the two to have taken it off, since a read is paying egress as well.

### By record size

Marginal cost per million operations, excluding rent:

| Record size | GB per million | Read | Write |
| --- | --- | --- | --- |
| 100 B | 0.10 | $0.010 | $0.005 |
| 1 KiB | 1.02 | $0.100 | $0.049 |
| 10 KiB | 10.2 | $1.00 | $0.49 |
| 100 KiB | 102 | $10.00 | $4.90 |
| 1 MiB | 1,049 | $102.80 | $50.35 |
| 16 MiB | 16,777 | $1,644.15 | $805.30 |

The rates behind every row:

```
read  = $0.09 egress +      no hop + $0.008 ALB = $0.098 per GB
write =        free  + $0.04 copies + $0.008 ALB = $0.048 per GB
```

The copies are the two other zones, at $0.02 a gigabyte each.

**Below about 10 KiB the rent dominates and the record size barely registers;
above about 100 KiB the bytes dominate and the rent barely registers.** At
100 req/s a 1 KiB read is $0.56 per million and a 1 MiB read is $103 per
million — a thousandfold in size, a hundred-and-eightyfold in price, because the
rent stops mattering.

### The CPU term

The one term that is not a byte, and the one most likely to be missed. It has a
clean form: surplus credits are $0.05 per vCPU-hour, so

```
CPU cost per million requests = $0.0139 × CPU milliseconds per request
```

and it is zero until the cluster leaves its baseline of 0.6 vCPU-hours an hour
(three instances × 0.2). That baseline is what fixes where the charge starts:

| CPU per request | Free up to | Then, per million requests |
| --- | --- | --- |
| 0.5 ms | 1,200 req/s | $0.007 |
| 1 ms | 600 req/s | $0.014 |
| 2 ms | 300 req/s | $0.028 |
| 5 ms | 120 req/s | $0.069 |
| 10 ms | 60 req/s | $0.139 |

Per request it is small — comparable to the marginal bytes of a 1 KiB read. It
is alarming in aggregate only because saturation is a plateau rather than a
slope: [$0.09 an hour per instance](#cpu-credits) is the ceiling, $197 a month
across the database tier, and a stack that is CPU-bound is paying it whether the
requests are 1 KiB or empty.

**CPU milliseconds per request is the one input here that has to be measured
rather than derived**, and it is also what sets the
[capacity ceiling](#the-ceiling). `perf/read.sh` and `perf/write.sh` against a
deployed stack give both at once.

### Storage

There is **no per-byte storage charge**. EBS bills the volumes the launch
template asked for, so the storage line is $16.70 a month — 180 GB at
$0.0928 — whether the database holds nothing or fills every disk. The price per
gigabyte *stored* is therefore not a price at all but a division, and it falls
as the disks fill:

| Live data | EBS line ÷ data | The whole stack ÷ data |
| --- | --- | --- |
| 10 GB | $1.67 / GB-month | $12.06 / GB-month |
| 20 GB | $0.84 / GB-month | $6.03 / GB-month |
| 40 GB | $0.42 / GB-month | $3.01 / GB-month |
| 60 GB | $0.28 / GB-month | $2.01 / GB-month |

**Every record is on three disks**, one in each zone, so a gigabyte of records
is three gigabytes of the "live data" column. Read the table as disk and then
divide the answer by three to price what was actually stored: 40 GB of disk is
about 13 GB of records at $1.26 a gigabyte-month of them, or $9.03 counting the
whole stack against them.

The two columns answer different questions, and **they must not be added to the
per-request prices above** — that would charge the fixed cost twice. Use the
left column when the stack is serving an API and the rent is already amortised
over requests; use the right when the stack exists to hold data and there is
nothing else to charge it to. On the right, at a realistic fill,
[a gigabyte here costs](#what-a-gigabyte-does-not-buy) more than a hundred
times what S3 charges — and S3's is durable.

Practical capacity is well under 180 GB, and a third of what fits is what can be
held: only the three database instances hold records, their volumes are *root*
volumes shared with the OS, the docker image and the logs, RocksDB needs
compaction headroom rather than a full disk, and every record is on all three —
so **40 GB of live data across the cluster is a realistic working figure**, and
that is about 13 GB of records. The $0.42 row is the one to quote per gigabyte
of disk, and three times it per gigabyte of records. RocksDB compresses, so that
is 40 GB after compression, not 40 GB of values as written.

Filling it produces no bill at all. It produces
[`storage_error` or `write_stalled`](/database/reference#errors).

### Putting it together

A blended figure, 80% reads and 20% writes at 1 KiB, which is the shape most
services have:

| Sustained load | Per million requests | Total monthly |
| --- | --- | --- |
| 1 / second | $45.97 | $120.82 |
| 10 / second | $4.68 | $122.91 |
| 100 / second | $0.549 | $144.20 |
| 250 / second | $0.274 | $179.58 |

**Two and a half orders of magnitude more traffic costs 49% more money.** That
is the whole economics of this stack in one line: it is rent, and the API
operations are nearly free until the records get large or the CPU runs out.

To price a workload that is not this one:

```
monthly = 120.58                                   fixed
        + reads  × size_GB × 0.098                 egress + ALB
        + writes × size_GB × 0.048                 copies into two zones + ALB
        + (reads + writes) × cpu_ms × 1.39e-8      credits, if above baseline
        - 9.00                                     first 100 GB egress, if unspent
```

Storage adds nothing to that until it runs out.

## What a request adds

Four marginal charges, in the order they usually matter.

### CPU credits

`t3.micro` is a burstable instance, and the template sets no
`CreditSpecification`, so it takes the T3 default: **`unlimited`**. A T3 in
`unlimited` mode does not throttle to its baseline when it runs out of credits.
It keeps running at full speed and bills the surplus.

| | |
| --- | --- |
| `t3.micro` | 2 vCPUs, baseline 10%, earning 12 credits an hour |
| Baseline | 0.2 vCPU-hours an hour |
| Fully saturated | 2.0 vCPU-hours an hour |
| Surplus | 1.8 vCPU-hours an hour × $0.05 = **$0.09 an hour** |

An instance pinned at 100% costs **$0.09 an hour in credits on top of a $0.0118
instance** — nearly eight times its own price. Three saturated database
instances are $197 a month, which is more than the entire fixed cost of the
stack, and they are still `t3.micro`s.

This is the single largest cost risk in the template, and it is invisible in the
resource list. Two things follow from it:

- **Sustained load is where the money is.** A stack answering a request a second
  never leaves its baseline and never pays a credit. A stack under the load
  `perf/write.sh` generates leaves it immediately and bills for every minute it
  stays there.
- **The choice is a bill or a cliff.** Setting `CreditSpecification` to
  `standard` caps the charge and throttles the instance to 10% of two vCPUs
  instead, which for a database is a worse failure than a bigger invoice —
  RocksDB compaction falls behind, and the symptom is
  [`write_stalled`](/database/reference#errors) rather than a slow answer.
  A non-burstable instance type is the honest fix for a stack expected to run
  hot, and `InstanceType` is a parameter for exactly that.

### Bytes out to the internet

$0.09 a gigabyte, after the first 100 GB a month, and it is charged on what
leaves — so **reads pay and writes do not.** Data in from the internet is free,
which means the body of a `PUT` crosses the boundary for nothing.

At the [16 MiB value limit](/database/reference#limits), one read costs
$0.0015 in egress, and the free 100 GB is about 5,960 of them. At a kilobyte a
record it takes a hundred million reads to reach the same place.

### Bytes across an availability zone

The [auto scaling group](/deployment/database#the-auto-scaling-group) is three
instances across three subnets, which is one per availability zone. Anything
one node sends another therefore crosses a zone boundary and is charged
**$0.01 leaving and $0.01 arriving — $0.02 a gigabyte.**

Reads cross nothing. Three instances in three zones is
[one copy of every key in each of them](/database/cluster#one-copy-in-every-zone),
so whichever node the load balancer picks holds the key and answers it where it
lands. That holds only while a zone has one node in it: a fourth instance puts
two in a zone, and half that zone's reads are then answered by its neighbour.

```
read  cross-AZ cost = 0                          while a zone is one node
write cross-AZ cost = 2 × $0.02 = $0.04 per GB   of record payload
```

A read that misses is the exception: a key the node holds nothing for is
[asked of the other copies](/database/cluster#what-a-write-and-a-read-do), which
is a few hundred bytes across a zone and nothing at all against a value.

**A write pays for the copies, and it is the only marginal byte charge a write
has.** Against $0.09 a gigabyte of egress, that is a write costing about four
ninths of what a read of the same bytes costs — before replication a write paid
$0.0133 and a read $0.1033, so what a copy in every zone actually cost is
$0.027 a gigabyte written, and what it bought is a read that never leaves the
zone it landed in and a record that outlives the loss of two of them.

Scans are the exception, and they are worth pricing separately.
[Every node is asked for the same range](/database/cluster#scans-across-a-cluster),
each returns up to the full `limit`, and the merge cuts what is over it — and
with a copy in every zone, what the three of them return is largely the same
records three times over. A thousand-record page can therefore move **three
thousand records' worth of bytes, two thousand of them across zone boundaries,
to return one thousand.**
Per byte delivered, a paged scan is the most expensive read in the API, and
narrowing it with `from` and `to` costs less than raising the `limit`.

Between the tiers, the traffic is nothing: each node
[renews its lease and re-reads the membership](/database/cluster#membership)
roughly every three seconds, which is a few kilobytes a tick and a few gigabytes
a month across the whole cluster — under a dollar, and mostly cross-AZ. The
load balancer's health checks are free: cross-zone traffic between an ALB and
its targets is not charged.

### Load balancer capacity units

An LCU is the **maximum** of four dimensions, not their sum, and it is billed at
$0.008 an hour:

| Dimension | One LCU is | What asyncdb does to it |
| --- | --- | --- |
| Processed bytes | 1 GB / hour | Request plus response, both directions |
| New connections | 25 / second | One per request without keep-alive; almost none with it |
| Active connections | 3,000 / minute | Long-lived clients sit here instead |
| Rule evaluations | 1,000 / second | Zero — [one default rule](/deployment/database#the-load-balancer), and the first ten are free |

Which dimension binds depends entirely on the size of a record, and the two ends
are far apart:

- At **16 MiB values**, 64 requests an *hour* is a gigabyte, so one large write a
  minute is already an LCU. Bytes bind long before anything else.
- At a **kilobyte**, it takes about 700 requests a second to reach a gigabyte an
  hour. Connections bind first, by a wide margin.

The practical consequence is one line: **keep the connection open.** Without
keep-alive, 250 requests a second is 10 LCU on the connection dimension —
$58 a month, half the fixed cost of the stack, for connection setup. With
keep-alive the same traffic sits under 1 LCU and costs $5.84. The server is
keep-alive by design and the nodes
[hold their connections to each other open](/database/cluster#what-each-endpoint-does-in-a-cluster);
a client that opens a connection per request throws that away and is billed for
it. This is also why `perf/harness.sh` measures over persistent connections —
it reports `num_connects` precisely so a run that is secretly reconnecting is
visible.

## What each endpoint costs

Beyond its share of the rent. `V` is the size of the value, `P` a page of a
scan, and "small" is a few hundred bytes of headers and JSON.

| Endpoint | Forwarded to | Cross-AZ bytes | Egress bytes | Marginal cost |
| --- | --- | --- | --- | --- |
| `GET /table/{t}/key/{k}` | nobody, unless the key is not here | none, or small on a miss | V | V × $0.09/GB |
| `HEAD /table/{t}/key/{k}` | nobody, unless the key is not here | none, or small on a miss | small | LCU only |
| `PUT /table/{t}/key/{k}` | the copy in **every** zone | 2 × V | small | V × $0.04/GB |
| `DELETE /table/{t}/key/{k}` | the copy in **every** zone | 2 × small | small | LCU only |
| `GET /table/{t}/key` | **every** node | up to 2 × P | P | P × $0.13/GB |
| `DELETE /table/{t}/key` | **every** node | 2 × small | small | LCU only |
| `PUT`/`DELETE` `/table/{t}` | **every** node | 2 × small | small | LCU only |
| `GET /table`, `GET /table/{t}` | nobody | none | small | LCU only |
| `GET /health` | nobody | none | small | LCU only |

The $0.09 of a read is egress and nothing else, because the node that was asked
holds the key; the $0.04 of a write is the value crossing into the two other
zones; the $0.13 of a scan is $0.09 plus the two full pages that cross a zone to
be thrown away.

Two readings of that table:

- **Everything that is not a value is free at the margin.** Deletes, existence
  checks, table operations and health checks move a few hundred bytes. They cost
  their share of the rent, some CPU, and nothing else — including the table
  fan-out, which touches every node and still moves nothing.
- **A write's only marginal charge is its copies**, at $0.04 a gigabyte, because
  ingress is free. Writes are still cheaper than reads of the same bytes — by a
  bit over two, rather than the seven it was before every zone kept a copy —
  which is the opposite of how a managed key-value store prices them.

## A worked example

A service writing 50 records a second and reading 200, all a kilobyte, over
keep-alive connections:

| Line | Arithmetic | Monthly |
| --- | --- | --- |
| Fixed cost | as above | $120.58 |
| Egress | (538 GB − 100 free) × $0.09 | $39.44 |
| Cross-AZ | 2 copies × 135 GB written × $0.02 | $5.38 |
| Load balancer LCU | 0.92 GB/hour → 1 LCU × 730 | $5.84 |
| CPU credits | assumed within baseline | $0.00 |
| | | **$171.24** |

250 requests a second is 657 million requests a month, so that is **$0.26 per
million requests**, of which $0.18 is the rent. Only the 131 million writes
cross a zone, and they cross it twice each; the 526 million reads are answered
where they land. The same traffic without keep-alive is $223, and the same
traffic hot enough to saturate three `t3.micro`s is $368 — the load pattern moves the bill by more than the load does.

## What a gigabyte does not buy

The [$0.42 a gigabyte-month](#storage) above is a real number and a poor
comparison, because it is not buying what a storage price usually buys.

There is nothing to pay for beyond the volumes — and little to *have*. No
snapshots, no backup vault, no cross-region copy, and no log to recover from:
what there is is
[a copy of every record in each zone](/database/cluster#one-copy-in-every-zone),
on three instances' root volumes, which is three thirty gigabyte disks holding
one keyspace. That survives an instance the auto scaling group replaces — the
other two zones still hold every record — and it does not make the stack
[durable](/deployment/#what-this-stack-does-not-do): the copies are written
together and lost together when the stack is, and a record written to a zone
whose disk is then gone was never anywhere else. That is a very cheap way
to store a gigabyte and a very poor way to keep one, and any comparison against
S3 at $0.024 or DynamoDB at $0.28 is comparing against a durable gigabyte.

The gp3 IO is the part that is genuinely included: 3,000 IOPS and 125 MB/s come
with the volume rather than as a line item, which is part of
[why gp3 replaced gp2](/deployment/database#the-root-volume) — the IO that was
being rationed by credits is now free and the volume is slightly cheaper.

And the ceiling is a wall, not a bill. The volume is the *root* volume, shared
with the OS, the docker image store and the logs; RocksDB is an LSM tree and a
compaction needs room for its output before it can release its input, so
sustained writing wants headroom rather than a full disk. Reaching the end of it
produces [`storage_error` or `write_stalled`](/database/reference#errors) — a
broken database rather than an expensive one.

## The ceiling

Since the fixed cost falls per request as the stack gets busier, the cost per
request is really a question about capacity, and the capacity is three
`t3.micro`s: two vCPUs and a gigabyte of memory each, with a
[thread pool sized to `hardware_concurrency()`](/deployment/database#the-launch-template)
and therefore two threads. A 16 MiB value is read whole into memory to be
served, so concurrent large reads are bounded by that gigabyte long before they
are bounded by anything else.

**This page does not assert a throughput number, because the stack measures its
own.** `perf/read.sh` and `perf/write.sh` drive a running deployment and report
latency percentiles, and
[the release runs them against the real stack](/pipeline/release) after the
Postman collection and the browser journeys. That is the number to divide the
fixed cost by, and it is worth re-measuring rather than inheriting: it moves
with `InstanceType`, with value size, and with whether the load is hot enough to
be paying for credits while it is measured.

Nothing scales on its own — `DesiredCapacity` is 3 with
[no policy and no alarm](/deployment/#what-this-stack-does-not-do) — so more
capacity is a hand on `MinSize` and `MaxSize`, and
[growing the cluster is a thing to do deliberately](/database/cluster#what-this-is-not)
because a key that changes owner is a key the new owner does not have. Adding a
fourth node adds $0.0118 an hour and a public address, and puts two nodes in one
availability zone — which is where a node stops holding every key and half that
zone's reads start being answered by its neighbour instead.

## What the pipeline costs

The [release](/pipeline/release) creates the whole stack, waits for three nodes,
runs the Postman collection, the Playwright journeys and both load scripts, and
deletes it again. At $0.165 an hour, **a stack standing for half an hour costs
about eight cents** — EC2 bills per second past a one minute minimum, and EBS
and the public addresses are prorated the same way.

Two footnotes on that:

- The load scripts are the part that saturates the instances, so a release pays
  some CPU credits on top. Over half an hour it is cents.
- A `create-stack` that fails because
  [one is already standing](/deployment/#what-this-stack-does-not-do) leaves
  that stack alone — and running. A stack forgotten after a failed build is
  $120 a month, and the ALB's fixed name means you will find out the next time
  a release tries to deploy.

ECR holds one image per released tag at $0.10 a GB-month and nothing prunes
them, so the repository grows by an image per release forever. It is small
money and it is unbounded; a lifecycle policy keeping the last few tags is the
one-line fix, and it is not in this repository.

## What is not costed here

- **Support, tax and discounts.** List price, no Savings Plan, no Reserved
  Instance, no Spot. The database tier is stateless enough for Spot in
  principle and the etcd tier is not, but the template asks for neither.
- **The account's other free tiers.** The 100 GB egress allowance is
  account-wide, not this stack's, so a busy account has already spent it.
- **CloudWatch.** Basic EC2 and ALB metrics are free and the template enables
  nothing else. Turning on detailed monitoring, access logs to S3 or a log
  driver on the container all add lines that are currently zero.
- **The build itself.** GitHub Actions minutes for the image build, which is
  where [every test runs](/pipeline/), are not an AWS charge.
- **A second region.** There is not one:
  [the registry and the region are literals](/deployment/#the-address) in the
  user data.
