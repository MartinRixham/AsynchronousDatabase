#! /usr/bin/env bash

# The replication factor is decreased, and then increased again.
#
#   doc/runbook/index.md                            a read needs one copy, a write needs every copy
#   doc/runbook/membership.md#the-membership-is-wrong    fewer zones than the deployment has
#   doc/runbook/rebuild.md                          a node fills itself in on the way up
#
# The database tier is given two of the three private subnets instead of three, by a stack update.
# A zone holds exactly one copy of the keyspace, so the number of subnets the group spans **is** the
# replication factor: three copies become two, and the six instances rebalance into three per
# remaining zone. Nothing is said to an instance about any of it — a node reads its own zone out of
# IMDS, so where the group puts it is what it is.
#
# It is the deliberate half of zone-lost. There a zone is cut off and comes back with its copy; here
# the copy is taken away on purpose and the instances holding it are terminated, so putting the zone
# back is a copy that has to be built rather than one that was waiting.
#
# Both moves redraw the split inside the zones that stay, and both are asked of the stores
# themselves: a zone that gains a node has a node that has to fetch what it now owns, and a zone
# that loses one has a node that has to be handed what the other let go. Neither is visible through
# the load balancer, which answers a read from whichever copy has the key.
#
# **Only the database tier moves.** The etcd group still spans three zones, so nothing here costs
# quorum, and the membership this is asserted against is one every node agrees on.
#
# Two is the floor and three is the ceiling: there is no fourth subnet to grow into, so the increase
# is asserted on the way back rather than as a fault of its own.
#
#   CHAOS_RESIZE   how long a rebalanced group is given to reach the new shape   1800 seconds

source "$(dirname "$0")/harness.sh"

banner "A zone is retired" "Two copies carry the keyspace, and the third is rebuilt when it returns."

setup
seed

# Rebalancing out of a zone is slower than changing a capacity: the group launches the replacement
# before it terminates what it is replacing, one at a time, and MaxSize allows one spare.
resize=${CHAOS_RESIZE:-1800}

expect "$(shape)" '[6,3,[2]]' "the tier is six nodes in three zones of two"

before=$(instances asyncdb | awk '{ print $2 }' | sort -u)

inject()
{
	echo "  Updating $stack to span two availability zones."

	stack_update Zones=2
}

heal()
{
	stack_update Zones=3
}

preflight()
{
	may_resize Zones=2
	may_run $(instances asyncdb | cut -f1)
}

fault_start || { verdict; exit 1; }

await '(.nodes | length) == 6 and (.zones | length) == 2 and ([ .zones[] | length ] | unique) == [3]' \
	"$resize" "the tier is six nodes in two zones of three"

# The design claim, and the reason a retired zone costs nothing to read: the two zones that are left
# were each holding a whole copy already, and neither of them lost a node — the group only ever
# terminates in the zone it is leaving. What they gained is a third node apiece, which rebuilt what
# it was about to own before it registered.
expect_readable 60 5 "every seeded record can still be read from the two copies that are left"
printf '  ---- reads with two copies: %s\n' "$(codes)"

# Which zone the group left, taken from where the instances actually are rather than from the
# template: the subnet named in the parameter is the one that is no longer offered, and the zone
# that empties is whichever the group was balancing into it.
retired=$(comm -23 <(echo "$before") <(instances asyncdb | awk '{ print $2 }' | sort -u))

expect "$(echo "$retired" | grep -c .)" 1 "exactly one zone was emptied — ${retired:-none was}"

# And a write needs every copy of the membership as it now stands, which is two.
expect_writes 20 "every write is taken by the two copies that are left"
expect "$(scan_status)" 200 "a scan is answered by a zone of three"

# Both zones went from two nodes to three, so a third of each zone's copy changed hands inside it.
expect_copies "with the tier in two zones"

fault_stop

# Putting the zone back is the increase, and it is two instances that have never held anything: the
# ones that were there went with the retirement.
await '(.nodes | length) == 6 and (.zones | length) == 3 and ([ .zones[] | length ] | unique) == [2]' \
	"$resize" "the tier spans three zones again, two nodes to a zone"

expect_writes 20 "every write is taken by three copies again"
expect_round_trip 10 "a key written once the third zone joined reads back what was written"

# The zone that came back has to hold a copy, and the two that shed a node have to hold the whole of
# theirs between the two that are left. This is the one place both halves are asserted at once: a
# zone was built from nothing and two zones were redrawn, by the same stack update.
expect_copies "once the third zone is back"

# A membership of three zones is three copies only if the new one holds anything, and no assertion
# made through the load balancer can see the difference: a read of a key this zone is missing is
# answered by the copies that have it. So the node is asked what is in its own store.
returned=$(instances asyncdb | awk -v z="$retired" '$2 == z { print $1 }')
joined=$(echo "$returned" | head -1)

if [ -z "$joined" ]; then
	result 1 "the retired zone $retired came back with instances of its own"
else
	result 0 "the zone $retired came back with instances of its own — $(echo "$returned" | tr '\n' ' ')"

	tables=$(ssm_run "$joined" 'curl -s --max-time 5 http://localhost:8080/table')

	case $tables in
		*"\"$table\""*) result 0 "$joined holds the table in its own store, so the third copy is real" ;;
		*) result 1 "$joined holds the table in its own store — it answers ${tables:-nothing}" ;;
	esac

	printf '  ---- %s says: %s\n' "$joined" \
		"$(ssm_run "$joined" "$(container_logs) | grep -i rebuil | tail -1")"
fi

# What the round trip of it cost the seed. The zones that were left shed a node each when the third
# came back, and what only that node held went with it — reported for the same reason nodes-removed
# reports it: what no node has, no pass can fetch.
absent=$(readable 60 3)

printf '  ---- of 60 seeded keys once all three zones are back: %s are held by no copy\n' "$absent"
expect_codes '^(2|404)' "a key that went with an instance is refused as not found and not as an error"

verdict
