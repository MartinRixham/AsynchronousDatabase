# The network

One VPC, three private subnets holding both tiers, three public subnets holding
nothing but the load balancer, and an egress-only internet gateway where a NAT
gateway would otherwise be. **No instance has a public IPv4 address and nothing
outside the VPC can open a connection to one.** What an instance can do is open
one outwards — over IPv6, and over nothing else.

```
VPC 10.0.0.0/16 and an Amazon /56   DNS support and DNS hostnames on
 │
 ├─ PublicSubnet1   10.0.10.0/24            AZ 0  ┐
 ├─ PublicSubnet2   10.0.11.0/24            AZ 1  ├─ the load balancer, alone
 ├─ PublicSubnet3   10.0.12.0/24            AZ 2  ┘
 │     PublicRouteTable     0.0.0.0/0 → InternetGateway
 │
 ├─ PrivateSubnet1  10.0.0.0/24  and a /64  AZ 0  ┐  six database instances
 ├─ PrivateSubnet2  10.0.1.0/24  and a /64  AZ 1  ├─ and three etcd instances,
 ├─ PrivateSubnet3  10.0.2.0/24  and a /64  AZ 2  ┘  each with both families
       PrivateRouteTable    ::/0 → EgressOnlyInternetGateway, and no 0.0.0.0/0
```

The availability zones are `Fn::Select` of index 0, 1 and 2 over
`Fn::GetAZs ""`, which is the zones of whichever region the stack is being
created in. Three of them are taken, so **the template needs a region with at
least three availability zones**; in one with two, `Fn::Select` of index 2 fails
at create time.

The IPv6 side is one `AWS::EC2::VPCCidrBlock` with `AmazonProvidedIpv6CidrBlock`,
which is a `/56` AWS picks, and `Fn::Cidr` cuts the first three `/64`s of it for
the private subnets. They set `AssignIpv6AddressOnCreation`, so **every instance
of both tiers launches with an IPv6 address as well as its private IPv4 one**,
and they carry `"DependsOn": "Ipv6CidrBlock"` because a subnet cannot be given a
slice of a block the VPC has not been given yet, and `Fn::GetAtt` of
`VPC.Ipv6CidrBlocks` does not tell CloudFormation that. The public subnets have
no IPv6 at all: the load balancer is `ipv4`, and giving them a block would be
address space nothing addresses.

Nothing inside the VPC uses those addresses. A node advertises
`ASYNCDB_NODE=http://$PRIVATE_IP:8080` off `local-ipv4`, etcd peers by IPv4, and
the load balancer reaches its targets the same way. **IPv6 here is the route
out and nothing else** — which is why the security groups still name IPv4
sources, and why the ingress rules did not change.

The private subnets keep the low `/24`s — `10.0.0.0`, `10.0.1.0` and `10.0.2.0`
— and the public subnets took new blocks rather than the other way round. The
reason was that the etcd addresses were literals in an `Etcd` mapping and those
instances were what moved; [the addresses are gone](/deployment/etcd) and the
CIDRs stayed, because renumbering a subnet replaces it.

`PublicRoute` carries `"DependsOn": "AttachGateway"`, which is one of the six
explicit dependencies in the template. It has to be there: a route to a gateway
that is not yet attached to the VPC is an error, and CloudFormation cannot infer
the ordering from a `Ref` because the route names the gateway, not the
attachment. The others are the three private subnets on `Ipv6CidrBlock` above,
and the two auto scaling groups on [the route out](#the-route-out) below.

`PrivateRouteTable` still has no `0.0.0.0/0` at all — the only route it holds
that leaves the VPC is `::/0`. There is one of it rather than one per zone,
because an egress-only internet gateway is a VPC-wide thing the way a NAT
gateway is a zonal one, so there is nothing zone-specific in it to get wrong.

## Why the instances are private

Both tiers pull as they start — the database instances from ECR, the etcd
instances from `quay.io` — and the pulls go out over **an egress-only internet
gateway rather than a NAT gateway**. What a private subnet is allowed to reach is
the difference between the three answers there: a NAT gateway is a route to all
of the internet and back through the translation it holds, VPC endpoints are a
route to the named AWS services and nothing else, and an egress-only internet
gateway is all of the internet in one direction only. It is stateful the way a
NAT gateway is — a reply to a connection an instance opened comes back, and
nothing that was not asked for gets in — and it does no translation, because
there is nothing to translate: the addresses on the far side of it are the
instance's own.

**A route off the instance is what `chaos/` needs.** Four of its ten
experiments inject their fault through the SSM agent, and `node-latency`
installs `tc` from the distribution's repositories before it does anything, so
without one four of [the runbook's](/runbook/) failure modes would have no
test.

**It is also the cheap answer.** Six interface endpoints in three availability
zones would be eighteen endpoint-hours an hour and
[about half the fixed cost of the stack](/deployment/cost#standing-still), where
an egress-only internet gateway is free. What that costs is honest to say: an
instance can open a connection to any address on the IPv6 internet, rather than
to seven named services. Nothing can open one to an instance, which is the half
that matters most and the half the gateway guarantees rather than leaves to a
rule.

`EnableDnsSupport` and `EnableDnsHostnames` are on, and they are what resolves
the public names to the AAAA records behind them.
resolves the public names to the AAAA records behind them.

## The route out

`EgressOnlyInternetGateway` is attached to the VPC, and `PrivateRoute` is the
one route that names it: `::/0` in `PrivateRouteTable`. That is the whole of it
— no addresses, no ENIs, no zones, and nothing to size.

What goes over it, and what each thing had to be told to use it:

| Goes out | To | How |
| --- | --- | --- |
| `aws ecr get-login-password` and `aws ec2 describe-instances` | `ecr.eu-west-2.api.aws`, `ec2.eu-west-2.api.aws` | `AWS_USE_DUALSTACK_ENDPOINT=true` in both user data scripts |
| `docker pull` | `332187735950.dkr-ecr.eu-west-2.on.aws` | The registry name in `REGISTRY_URL`, which is [ECR's dual-stack form](https://docs.aws.amazon.com/AmazonECR/latest/userguide/ecr-requests.html) and not `dkr.ecr…amazonaws.com` |
| The SSM agent — Session Manager, and the Run Commands `chaos/` injects with | Systems Manager's dual-stack endpoints | `UseDualStackEndpoint` in `/etc/amazon/ssm/amazon-ssm-agent.json`, written by the user data before the agent is restarted |
| `dnf`, which is what `node-latency` installs `tc` with | The Amazon Linux repositories in S3 | Nothing. On EC2 they are already the `s3.dualstack` names |

**Every one of those had to be named differently.** An AWS service endpoint is
IPv4-only unless it is asked for in its dual-stack form, so a stack whose only
route out is IPv6 does not work by leaving the defaults alone: it fails at the
first `aws` call, an instance boots with no container, and the load balancer
answers 502 while the auto scaling group replaces it with another that does the
same. That is the failure to look for after any change to a user data script,
and [it looks exactly like a missing image tag](/runbook/deployment).

It does not always fail *quickly*, either. An instance still has an IPv4 default
route — to the subnet's own router, which has nowhere to forward to — so an IPv4
packet aimed at the internet is dropped rather than refused, and a client that
tries the A record first waits out its connect timeout instead of failing and
moving on. Both families are in DNS for every name above, and the resolver
prefers the AAAA when the instance has a global IPv6 address, which it does; but
a call that hangs for a minute and then works is this, and not a slow service.

The SSM agent needs a version that has that setting at all — 3.3270.0 or later —
and nothing here pins one: the [base image](/deployment/#parameters) is
whatever the public parameter resolves to on the day. An older agent ignores
`UseDualStackEndpoint` and goes on calling an endpoint it cannot reach, so that
is the first thing to check if Session Manager stops answering after a base
image change. `/var/log/amazon/ssm/amazon-ssm-agent.log` says which endpoint it
tried.

`ec2:DescribeInstances` is the call the stack could least do without:
**an instance that cannot make it cannot find out what cluster it is in.** The
etcd tier would not form, and the database tier would come up
[owning the whole keyspace each](/database/cluster#turning-it-on).

Two `DependsOn` follow from all of this:

- **`AutoScalingGroup` depends on `PrivateRoute`.** Nothing in a launch template
  references a route, so CloudFormation would otherwise be free to launch six
  instances into a subnet that has no way out yet, and each of them would boot
  with no container.
- **`EtcdAutoScalingGroup` depends on `PrivateRoute`, `EtcdClientIngress` and
  `EtcdPeerIngress`.** The route for the same reason, and the two rules because
  a launching etcd node asks the ones already running to admit it — over 2379,
  and then over 2380 for good. Neither is referenced by the launch template
  either.

The region is written into the user data twice over — `REGION=eu-west-2` and the
registry's own name — and the dual-stack endpoints are built from it, so it is
[the same one-region limit the stack always had](/deployment/#the-address).

## The security groups

| Group | Ingress | Egress |
| --- | --- | --- |
| `ALBSecurityGroup` | 80/tcp from `0.0.0.0/0` | everything, both families |
| `InstanceSecurityGroup` | 80/tcp from `ALBSecurityGroup` | everything, both families |
| `EtcdSecurityGroup` | 2379/tcp from `InstanceSecurityGroup` | everything, both families |

Three more rules cannot be written inline, because a group that names itself in
its own `SecurityGroupIngress` is a circular reference. They are separate
`AWS::EC2::SecurityGroupIngress` resources instead:

| Rule | Opens |
| --- | --- |
| `InstanceApiIngress` | 8080/tcp on `InstanceSecurityGroup`, from `InstanceSecurityGroup` |
| `EtcdPeerIngress` | 2380/tcp on `EtcdSecurityGroup`, from `EtcdSecurityGroup` |
| `EtcdClientIngress` | 2379/tcp on `EtcdSecurityGroup`, from `EtcdSecurityGroup` |

Everything is addressed by source group rather than by CIDR, which is what
keeps the rules right as instances come and go — and with
[both tiers now in groups](/deployment/etcd#the-group), every address in the
stack is one that came from a launch. The load balancer reaches port 80 on the
database instances, the database instances reach port 8080 on each other and
2379 on etcd, and the etcd instances reach 2379 and 2380 on each other — the
client port because that is where a launching node asks to be admitted, and the
peer port because that is where the raft is. Nothing else reaches any of it, and there is no rule anywhere that
names `0.0.0.0/0` as a source except the load balancer's port 80.

One thing is still open, and is worth saying out loud: **etcd has no TLS and no
authentication.** The port is closed to the internet, but within the VPC
anything holding `InstanceSecurityGroup` can read and write the membership — see
[what is left to the etcd tier](/deployment/etcd#what-is-left-to-it).

Egress is `IpProtocol: -1` to `0.0.0.0/0` **and to `::/0`** on all three, and
both halves have to be written out. A security group that is left alone allows
all egress over both families, but a group that names any `SecurityGroupEgress`
at all replaces that default with exactly what it names — so the `0.0.0.0/0`
rule these already had, on its own, would be a group that dropped every IPv6
packet, which is now every packet that leaves the VPC. The IPv4 half is a wide
rule over a small world, since `PrivateRouteTable` has nowhere to send it but
the other instances; the IPv6 half is the wide rule it looks like, and
[the route out](#the-route-out) is where that is argued.

## Getting onto an instance

There is no SSH: no port 22 in either security group and no `KeyName` on either
launch template. An instance with no public address is not reachable from
outside the VPC, there is no bastion in the template, and a key pair that cannot
be used is a prerequisite the stack does not need.

**Session Manager is the way in**, and it works the way everything else on an
instance does: the agent opens the connection outwards, over
[the route out](#the-route-out), to Systems Manager's dual-stack endpoints —
which is what the `UseDualStackEndpoint` line in the user data is for. Nothing
is opened towards the instance, by Session Manager or by anything else.
The database tier carries `AmazonSSMManagedInstanceCore` on `InstanceRole`; the
etcd tier carries it on `EtcdRole`, alongside the `ec2:DescribeInstances` it
[finds its peers with](/deployment/etcd#the-membership-the-instances-imply) and
the `AmazonEC2ContainerRegistryReadOnly` it
[pulls etcd with](/deployment/etcd#where-the-image-comes-from).

```bash
aws ssm start-session --target i-0123456789abcdef0
```

It is a better door than SSH — no key to distribute, no port open to anybody, and
the access is IAM and CloudTrail rather than a file on somebody's laptop.

## The API port is not the load balancer's

8080 is open **between database instances and nowhere else**. It is the port
nodes [forward to each other on](/database/cluster#which-node-owns-a-key), it
is not behind the nginx that serves the `/asyncdb` prefix, and it honours
`X-Asyncdb-Forwarded` from anyone who sends it. The load balancer's target group
is port 80, deliberately: a request from outside arrives at nginx, and only
asyncdb talks to asyncdb.

Port 80 is not a way round that. A request marked as forwarded is served where it
lands rather than sent on to the node that owns the key, so a client that set the
header itself would be answered by whichever instance the load balancer picked —
and answered wrongly, since that instance holds the key only when it happens to
own it. `server/server.conf` therefore clears the header on the way through:

```nginx
proxy_set_header X-Asyncdb-Forwarded "";
```

Only another node may say a request has been forwarded, and another node says it
to 8080.
