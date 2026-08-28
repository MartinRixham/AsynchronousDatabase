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
ECR, the etcd instances from `quay.io` — and a private subnet doing that needs a
NAT gateway, which is the most expensive thing
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
| `EtcdSecurityGroup` | 22/tcp from `0.0.0.0/0`, 2379/tcp from `InstanceSecurityGroup` | everything |

Two more rules cannot be written inline, because a group that names itself in
its own `SecurityGroupIngress` is a circular reference. They are separate
`AWS::EC2::SecurityGroupIngress` resources instead:

| Rule | Opens |
| --- | --- |
| `InstanceApiIngress` | 8080/tcp on `InstanceSecurityGroup`, from `InstanceSecurityGroup` |
| `EtcdPeerIngress` | 2380/tcp on `EtcdSecurityGroup`, from `EtcdSecurityGroup` |

Everything but SSH is therefore addressed by source group rather than by CIDR,
which is what keeps the rules right as instances come and go: the load balancer
reaches port 80 on the database instances, the database instances reach port
8080 on each other and 2379 on etcd, and the etcd instances reach 2380 on each
other. Nothing else reaches any of it.

Two things are still open, and are worth saying out loud:

- **SSH from anywhere**, on both instance groups. There is no bastion and no
  restriction to an office address; the key pair is the whole of the access
  control. One address, or Session Manager instead of a key pair, is the fix.
- **etcd has no TLS and no authentication.** The port is closed to the internet
  now, but within the VPC anything holding `InstanceSecurityGroup` can read and
  write the membership — see
  [what is left to the etcd tier](/deployment/etcd#what-is-left-to-it).

Egress is `IpProtocol: -1` to `0.0.0.0/0` on all three, written out rather than
left to the default, which is the same thing. Both tiers need it at boot: the
database instances to ECR and to the AWS CLI download, the etcd instances to
`quay.io`.

## The API port is not the load balancer's

8080 is open **between database instances and nowhere else**. It is the port
nodes [forward to each other on](/database/cluster#which-node-owns-a-key), it
is not behind the nginx that serves the `/asyncdb` prefix, and it honours
`X-Asyncdb-Forwarded` from anyone who sends it. The load balancer's target group
is port 80, deliberately: a request from outside arrives at nginx, and only
asyncdb talks to asyncdb.
