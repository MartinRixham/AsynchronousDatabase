# The network

One VPC, three public subnets, one route to the internet and three security
groups. There is no private subnet and no NAT gateway anywhere in the template:
every instance has a public address and talks to the internet directly.

```
VPC 10.0.0.0/16   DNS support and DNS hostnames on
 │
 ├─ PublicSubnet1  10.0.0.0/24   AZ 0  ┐
 ├─ PublicSubnet2  10.0.1.0/24   AZ 1  ├─ MapPublicIpOnLaunch: true
 ├─ PublicSubnet3  10.0.2.0/24   AZ 2  ┘
 │
 └─ PublicRouteTable    0.0.0.0/0 → InternetGateway
       associated with all three subnets
```

The availability zones are `Fn::Select` of index 0, 1 and 2 over
`Fn::GetAZs ""`, which is the zones of whichever region the stack is being
created in. Three of them are taken, so **the template needs a region with at
least three availability zones**; in one with two, `Fn::Select` of index 2 fails
at create time.

`PublicRoute` carries `"DependsOn": "AttachGateway"`, which is the one explicit
dependency in the template. It has to be there: a route to a gateway that is not
yet attached to the VPC is an error, and CloudFormation cannot infer the
ordering from a `Ref` because the route names the gateway, not the attachment.

## Why everything is public

Both tiers pull from the internet as they start — the database instances from
ECR, the etcd instances from `quay.io` and from the discovery service — and a
private subnet doing that needs a NAT gateway, which is the most expensive thing
that would be in this template. Public subnets with `MapPublicIpOnLaunch` are
the cheap way, and the security groups are then the only thing between the
instances and the internet.

`EnableDnsHostnames` is what gives the instances the public DNS names the load
balancer and the console show.

## The security groups

| Group | Ingress | Egress |
| --- | --- | --- |
| `ALBSecurityGroup` | 80/tcp from `0.0.0.0/0` | everything |
| `InstanceSecurityGroup` | 22/tcp from `0.0.0.0/0`, 80/tcp from `ALBSecurityGroup` | everything |
| `EtcdSecurityGroup` | 22/tcp from `0.0.0.0/0`, 2379–2380/tcp from `0.0.0.0/0` | everything |

The one rule that is right is the middle one: the database instances take HTTP
from the load balancer's security group and from nowhere else, by source group
rather than by CIDR, so it keeps working as instances come and go.

The rest are as open as they can be, and worth saying out loud:

- **SSH from anywhere**, on both groups. There is no bastion and no restriction
  to an office address; the key pair is the whole of the access control.
- **etcd from anywhere.** Ports 2379 and 2380 — the client port and the peer
  port — are open to `0.0.0.0/0`, and the etcd instances have public addresses
  and [advertise them](/deployment/etcd#the-launch-template). etcd is started
  with no TLS and no authentication, so this is an unauthenticated key-value
  store on the public internet, and the keyspace it holds is the cluster's
  membership: anyone can read who the nodes are, and anyone can write a node
  that does not exist into it, or delete the ones that do.

Tightening it is the same shape as the middle rule. 2379 belongs to
`InstanceSecurityGroup` as a source, 2380 belongs to `EtcdSecurityGroup` itself,
and 22 belongs to one address or to Session Manager instead of a key pair. What
the API port needs is on the [database tier](/deployment/database) page.

Egress is `IpProtocol: -1` to `0.0.0.0/0` on all three, written out rather than
left to the default, which is the same thing. Both tiers need it: to ECR, to
quay.io, to `discovery.etcd.io` and to SSM.

## The port the cluster would need

Nothing in any group allows 8080. That is the API port — the port nodes reach
each other on, and not the nginx in front of it — so as the template stands the
[forwarding](/database/cluster#which-node-owns-a-key) a cluster is made of has
nowhere to go. It would be one rule on `InstanceSecurityGroup`, 8080/tcp with
`SourceSecurityGroupId` naming the group itself, and `-p 8080:8080` on the
`docker run`.
