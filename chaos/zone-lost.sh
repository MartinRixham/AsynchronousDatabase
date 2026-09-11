#! /usr/bin/env bash

# A whole availability zone is gone.
#
#   doc/runbook/index.md          "A read needs one copy. A write needs a leader and every copy."
#   doc/runbook/membership.md#the-membership-is-wrong    fewer zones than the deployment has
#
# One private subnet is cut off from the other two availability zones. That subnet holds two of
# the six database nodes and one of the three etcd members, so the minority side loses quorum,
# stops renewing its leases and falls out of the membership; the majority side is left with four
# nodes in two zones, which is two whole copies of the keyspace.
#
# This is the claim at the top of the runbook, tested: every zone can be the one that is gone
# and a record is still read. Nothing is asked of the instances — the fault is a network access
# control list of the suite's own, on the subnet — which is why it works on a stack whose
# instances have no route to anywhere.

source "$(dirname "$0")/harness.sh"

banner "A zone is cut off" "Reads carry on from the other copies, and the membership loses a zone."

setup
seed
start_load

# The private subnet of a zone is the subnet an asyncdb instance in that zone is in. Nothing is
# read out of the template for this: the instance knows where it is.
zone=$(instances asyncdb | awk '{ print $2 }' | uniq | head -1)
subnet=$(instances asyncdb | awk -v z="$zone" '$2 == z { print $3 }' | head -1)
cut_off=$(instances asyncdb | awk -v z="$zone" '$2 == z { print $1 }')

# A node of one of the other two zones, which is where the claim this experiment makes is true.
witness=$(instances asyncdb | awk -v z="$zone" '$2 != z { print $1 }' | head -1)

# What the membership will be left with, worked out from the membership and not from the
# instances: the group balances across zones but drifts as instances are replaced, and an instance
# it has just launched is running and not yet registered. It is asked before the fault, because
# afterwards the load balancer answers with whichever side it routed to.
addresses=$(aws ec2 describe-instances --instance-ids $cut_off \
	--query 'Reservations[].Instances[].PrivateIpAddress' --output text \
	| tr '\t' '\n' | jq -Rsc 'split("\n") | map(select(length > 0))')

remaining=$(curl --fail --silent --max-time 10 "$base/health" \
	| jq --argjson cut "$addresses" \
		'[ .nodes[] | select([ $cut[] as $ip | select(contains($ip)) ] | length == 0) ] | length')

# The group may be at seven instances for a while — it is allowed to be, MaxSize is 7 — so what
# matters is that the zone has nodes to cut off and not exactly how many.
[ "$(echo "$cut_off" | wc -l)" -ge 2 ] \
	&& result 0 "the zone $zone holds $(echo "$cut_off" | wc -l) database nodes to cut off" \
	|| result 1 "the zone $zone holds too few nodes to be worth cutting off"
echo "  Cutting off $subnet in $zone, which holds $(echo "$cut_off" | tr '\n' ' ')"

inject()
{
	zone_cut "$subnet" "$zone"
}

heal()
{
	zone_heal
}

preflight()
{
	may_write_acls
}

fault_start || { verdict; exit 1; }

# Two zones is one copy of the keyspace gone. The nodes of the cut off zone cannot renew their
# leases against a single etcd member that has no quorum, so they leave the membership rather
# than sit in it refusing every write to their keys.
#
# It is asked of a node that is still on the majority side, and not of the load balancer: nothing
# takes a cut off node out of the load balancer until its own health check has failed twice, so
# until then it goes on answering — with a membership of one, which is the truth about itself and
# not about the cluster this is making a claim about.
await_node "$witness" "(.zones | length) == 2 and (.nodes | length) == $remaining" "$settle" \
	"a zone left the membership, leaving $remaining nodes and two copies"

load_report "while the zone was going away"

# The point of the whole design: a read needs one copy, and two remain.
# A node the fault cut off is still in the load balancer for as long as the health check takes to
# notice: it holds a membership of one, so it can order no write and says so, but the acl leaves
# its own zone alone and the load balancer node there goes on reaching it until it has failed the
# check twice. Until then it answers for keys it does not hold, which is
# doc/runbook/membership.md's own warning happening — so what is asserted here is the runbook's
# actual claim, that a record survives any one zone, and the 404s from the isolated side are
# reported beside it.
expect_readable 60 5 "every seeded record can still be read with a zone gone"
printf '  ---- reads with a zone gone: %s\n' "$(codes)"
expect "$(scan_status)" 200 "a scan falls back to a zone that is whole"

# And a write needs every copy of the membership as it now stands, which is two. It is waited for
# rather than asserted outright, because the isolated side refuses a write before the load
# balancer has stopped choosing it: what is claimed here is that the writes come back, and how
# long that takes is the health check's interval rather than anything this database does.
await_writes 20 "$settle" "every write is taken by the two zones that are left"

# The isolated nodes are still reachable over Run Command: the interface endpoints they go
# through have an interface in their own subnet, the fault is between zones, and the acl leaves
# IPv6 alone, which is how they reach Systems Manager. What they say about themselves is the
# cluster-of-one behaviour, reported rather than asserted: whether that path survives the fault
# is the network's business and not the database's.
for isolated in $cut_off; do
	echo "  $isolated says: $(node_health "$isolated" | jq -c '{nodes, zones, leads}' 2> /dev/null)"
done

fault_stop

await '(.zones | length) == 3 and (.nodes | length) == 6' "$recovery" \
	"the zone rejoined and the third copy is back"

expect_reads 40 "every read is answered once the zone is back"
expect_writes 20 "every write is taken once the zone is back"

# A zone that was cut off comes back with its copy, so nothing a client was told had been taken
# was anywhere but on all three of them throughout.
expect_load_kept "once the zone was back"

verdict
