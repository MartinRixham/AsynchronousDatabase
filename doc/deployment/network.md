# The network

One VPC, three private subnets holding both tiers, three public subnets holding
nothing but the load balancer, and six VPC endpoints where a NAT gateway would
otherwise be. **No instance has a public address and neither tier has a route to
the internet**: everything an instance needs at boot, it reaches inside the VPC.

```
VPC 10.0.0.0/16   DNS support and DNS hostnames on
 │
 ├─ PublicSubnet1   10.0.10.0/24  AZ 0  ┐
 ├─ PublicSubnet2   10.0.11.0/24  AZ 1  ├─ the load balancer, and nothing else
 ├─ PublicSubnet3   10.0.12.0/24  AZ 2  ┘
 │     PublicRouteTable     0.0.0.0/0 → InternetGateway
 │
 ├─ PrivateSubnet1  10.0.0.0/24   AZ 0  ┐  six database instances,
 ├─ PrivateSubnet2  10.0.1.0/24   AZ 1  ├─ three etcd instances,
 ├─ PrivateSubnet3  10.0.2.0/24   AZ 2  ┘  five interface endpoints
       PrivateRouteTable    local only, plus the S3 gateway endpoint
```

The availability zones are `Fn::Select` of index 0, 1 and 2 over
`Fn::GetAZs ""`, which is the zones of whichever region the stack is being
created in. Three of them are taken, so **the template needs a region with at
least three availability zones**; in one with two, `Fn::Select` of index 2 fails
at create time.

The private subnets keep the low `/24`s — `10.0.0.0`, `10.0.1.0` and `10.0.2.0`
— because [the etcd addresses are literals](/deployment/etcd#the-addresses) in
the `Etcd` mapping and those instances are what moved. The public subnets took
new blocks rather than the other way round.

`PublicRoute` carries `"DependsOn": "AttachGateway"`, which is one of the three
explicit dependencies in the template. It has to be there: a route to a gateway
that is not yet attached to the VPC is an error, and CloudFormation cannot infer
the ordering from a `Ref` because the route names the gateway, not the
attachment. The other two are the [endpoints](#the-endpoints) below.

`PrivateRouteTable` has no `0.0.0.0/0` at all. There is one of it rather than
one per zone, because with no NAT gateway to be in a zone there is nothing
zone-specific in it to get wrong.

## Why the instances are private

Both tiers used to sit in public subnets with `MapPublicIpOnLaunch`, because
both pull as they start — the database instances from ECR, the etcd instances
from `quay.io` — and the cheap way to let them was to give every instance a
public address. The security groups were then the only thing between nine
instances and the internet.

They are private now, and the pulls go through **VPC endpoints instead of a NAT
gateway**. The difference between the two is what a private subnet is allowed to
reach: a NAT gateway is a route to all of the internet, and endpoints are a
route to the named AWS services and nothing else. `quay.io` is not one of them,
so it is off the boot path for good — which is
[what the etcd AMI already bakes](/deployment/etcd#the-instances), and
is now a requirement rather than a saving.

**It is not the cheap answer.** Five interface endpoints in three availability
zones is fifteen endpoint-hours an hour, and
[they are about half the fixed cost of the stack](/deployment/cost#standing-still)
— more than a single NAT gateway would be, and more than the nine public
addresses they replaced. What the money buys is that nothing in either tier can
open a connection to the internet, in either direction: no address to reach, and
no route out to be reached over.

`EnableDnsSupport` and `EnableDnsHostnames` are what make it work at all. The
interface endpoints set `PrivateDnsEnabled`, which points the ordinary service
names — `api.ecr.eu-west-2.amazonaws.com` and the rest — at the endpoint's own
addresses inside the VPC, so **the user data is the same script it was**: it
still runs `aws ecr get-login-password` and `docker pull` against the public
names, and they resolve privately.

## The endpoints

| Endpoint | Type | Carries |
| --- | --- | --- |
| `S3Endpoint` | Gateway | The image layers, which ECR stores in S3 — a `docker pull` needs this as much as it needs `ecr.dkr` |
| `EcrApiEndpoint` | Interface | `aws ecr get-login-password`, which is `ecr:GetAuthorizationToken` |
| `EcrDockerEndpoint` | Interface | The `docker pull` itself |
| `SsmEndpoint`, `SsmMessagesEndpoint`, `Ec2MessagesEndpoint` | Interface | Session Manager, which is [the only way onto an instance](#getting-onto-an-instance) now |

The gateway endpoint is a route rather than an address: it is free, it is named
in `PrivateRouteTable`, and it works by prefix list. The five interface
endpoints are ENIs, one in each private subnet, so an instance reaches the one
in its own availability zone and pays nothing to cross a boundary.

Two `DependsOn` follow from all of this:

- **`AutoScalingGroup` depends on `S3Endpoint`, `EcrApiEndpoint` and
  `EcrDockerEndpoint`.** Nothing in a launch template references an endpoint, so
  CloudFormation would otherwise be free to launch six instances into a subnet
  that cannot yet reach ECR, and each of them would boot with no container.
- **Each `Etcd` instance depends on its `PrivateSubnetRouteTableAssociation`**,
  which is what its dependency on `PublicRoute` became.

`ServiceName` is `Fn::Sub` of `com.amazonaws.${AWS::Region}.…`, so the endpoints
follow the stack. The ECR registry and `--region eu-west-2` in the user data do
not, which is
[the same one-region limit the stack always had](/deployment/#the-address).

## The security groups

| Group | Ingress | Egress |
| --- | --- | --- |
| `ALBSecurityGroup` | 80/tcp from `0.0.0.0/0` | everything |
| `InstanceSecurityGroup` | 80/tcp from `ALBSecurityGroup` | everything |
| `EtcdSecurityGroup` | 2379/tcp from `InstanceSecurityGroup` | everything |
| `VpcEndpointSecurityGroup` | 443/tcp from `InstanceSecurityGroup` and from `EtcdSecurityGroup` | everything |

Two more rules cannot be written inline, because a group that names itself in
its own `SecurityGroupIngress` is a circular reference. They are separate
`AWS::EC2::SecurityGroupIngress` resources instead:

| Rule | Opens |
| --- | --- |
| `InstanceApiIngress` | 8080/tcp on `InstanceSecurityGroup`, from `InstanceSecurityGroup` |
| `EtcdPeerIngress` | 2380/tcp on `EtcdSecurityGroup`, from `EtcdSecurityGroup` |

Everything is now addressed by source group rather than by CIDR, which is what
keeps the rules right as instances come and go: the load balancer reaches port
80 on the database instances, the database instances reach port 8080 on each
other and 2379 on etcd, both tiers reach 443 on the endpoints, and the etcd
instances reach 2380 on each other. Nothing else reaches any of it, and there is
no rule anywhere that names `0.0.0.0/0` as a source except the load balancer's
port 80.

One thing is still open, and is worth saying out loud: **etcd has no TLS and no
authentication.** The port is closed to the internet, but within the VPC
anything holding `InstanceSecurityGroup` can read and write the membership — see
[what is left to the etcd tier](/deployment/etcd#what-is-left-to-it).

Egress is `IpProtocol: -1` to `0.0.0.0/0` on all four, written out rather than
left to the default, which is the same thing. It is a wide rule over a small
world now: with no route to the internet in `PrivateRouteTable`, the only places
a packet can go are the other instances, the endpoints and the S3 prefix list.

## Getting onto an instance

There is no SSH. Both tiers used to open 22 from `0.0.0.0/0` and set
`KeyName: asyncdb`, and both are gone: an instance with no public address is not
reachable from outside the VPC, there is no bastion in the template, and a key
pair that cannot be used is a prerequisite the stack no longer needs.

**Session Manager is the way in**, which is what the three `ssm` endpoints are
for. The database tier already carried `AmazonSSMManagedInstanceCore` on
`InstanceRole`; the etcd tier had no role at all and now has `EtcdRole`, which
carries that policy and nothing else.

```bash
aws ssm start-session --target i-0123456789abcdef0
```

It is a better door than the one it replaced — no key to distribute, no port
open to anybody, and the access is IAM and CloudTrail rather than a file on
somebody's laptop.

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
