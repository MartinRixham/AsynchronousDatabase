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
# and a record is still read. It is agentless — the fault is a network access control list the
# service writes, and nothing is asked of the instances — which is why it works on a stack whose
# instances have no route to anywhere.

source "$(dirname "$0")/harness.sh"

banner "A zone is cut off" "Reads carry on from the other copies, and the membership loses a zone."

setup
seed

# The private subnet of a zone is the subnet an asyncdb instance in that zone is in. Nothing is
# read out of the template for this: the instance knows where it is.
zone=$(instances asyncdb | awk '{ print $2 }' | uniq | head -1)
subnet=$(instances asyncdb | awk -v z="$zone" '$2 == z { print $3 }' | head -1)
cut_off=$(instances asyncdb | awk -v z="$zone" '$2 == z { print $1 }')

# The group may be at seven instances for a while — it is allowed to be, MaxSize is 7 — so what
# matters is that the zone has nodes to cut off and not exactly how many.
[ "$(echo "$cut_off" | wc -l)" -ge 2 ] \
	&& result 0 "the zone $zone holds $(echo "$cut_off" | wc -l) database nodes to cut off" \
	|| result 1 "the zone $zone holds too few nodes to be worth cutting off"
echo "  Cutting off $subnet in $zone, which holds $(echo "$cut_off" | tr '\n' ' ')"

duration=${CHAOS_ZONE_DURATION:-PT6M}

cat > "$work/template.json" <<EOF
{
	"description": "asyncdb chaos: one availability zone is cut off",
	"roleArn": "$role",
	"stopConditions": [ { "source": "none" } ],
	"tags": { "Name": "asyncdb-chaos" },
	"targets": {
		"Zone": {
			"resourceType": "aws:ec2:subnet",
			"resourceArns": $(arns subnet "$subnet"),
			"selectionMode": "ALL"
		}
	},
	"actions": {
		"disrupt": {
			"actionId": "aws:network:disrupt-connectivity",
			"parameters": { "duration": "$duration", "scope": "availability-zone" },
			"targets": { "Subnets": "Zone" }
		}
	}
}
EOF

fis_start "$work/template.json" || { verdict; exit 1; }
fis_await_running || { verdict; exit 1; }

start_probe

# Two zones is one copy of the keyspace gone. The nodes of the cut off zone cannot renew their
# leases against a single etcd member that has no quorum, so they leave the membership rather
# than sit in it refusing every write to their keys.
# What is left is what was there minus what was cut off, and neither number is two. The group
# balances across zones but drifts as instances are replaced, so a zone can hold three nodes and
# another one — a run that assumed two per zone asserted a membership that could never arrive.
remaining=$(( $(instances asyncdb | wc -l) - $(echo "$cut_off" | wc -l) ))

await "(.zones | length) == 2 and (.nodes | length) == $remaining" "$settle" \
	"a zone left the membership, leaving $remaining nodes and two copies"

stop_probe
probe_report "while the zone was going away"

# The point of the whole design: a read needs one copy, and two remain.
# A node the fault cut off is still in the load balancer: /health answers 200 whatever its
# membership says, so nothing deregisters it, and it answers for keys it does not hold. That is
# doc/runbook/membership.md's own warning happening — so what is asserted here is the runbook's
# actual claim, that a record survives any one zone, and the 404s from the isolated side are
# reported beside it.
expect_readable 60 5 "every seeded record can still be read with a zone gone"
printf '  ---- reads with a zone gone: %s\n' "$(codes)"
expect "$(scan_status)" 200 "a scan falls back to a zone that is whole"

# And a write needs every copy of the membership as it now stands, which is two.
expect_writes 20 "every write is taken by the two zones that are left"

# The isolated nodes are still reachable over Run Command, because the interface endpoints they
# go through have an interface in their own subnet and the fault is between zones. What they say
# about themselves is the cluster-of-one behaviour, reported rather than asserted: whether that
# path survives the fault is the network's business and not the database's.
for isolated in $cut_off; do
	echo "  $isolated says: $(node_health "$isolated" | jq -c '{nodes, zones, leads}' 2> /dev/null)"
done

echo "  Waiting for the fault to be removed."

fis_await_end $((recovery / 2)) > /dev/null

await '(.zones | length) == 3 and (.nodes | length) == 6' "$recovery" \
	"the zone rejoined and the third copy is back"

expect_reads 40 "every read is answered once the zone is back"
expect_writes 20 "every write is taken once the zone is back"

verdict
