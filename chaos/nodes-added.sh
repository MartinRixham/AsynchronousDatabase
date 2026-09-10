#! /usr/bin/env bash

# The partition factor is increased, and then decreased again.
#
#   doc/runbook/rebuild.md                          a node fills itself in on the way up
#   doc/runbook/rebuild.md#when-ownership-moves     and moves records when ownership moves
#   doc/database/cluster.md                         a key belongs to one of 256 partitions
#
# The database tier goes from six instances to nine by a stack update, which is three zones of
# three where it was three zones of two. The number of copies never moves — a zone holds one, and
# there are still three zones — so what changes is how many ways each zone splits the copy it
# holds, and that redraws the split inside every zone at once.
#
# A node that joins rebuilds what it is about to own before it registers, and the nodes it joined
# then give up what it took over — the clear down half of a reconcile, which waits until the new
# owner answers that it holds the key. **A leftover copy is what this experiment asserts against.**
# A zone holds one copy of the keyspace and its nodes split it, so a key in two stores of one zone
# is a node that kept what it stopped owning.
#
# It would stay invisible if the tier stayed wide, because nothing reads that copy. The shrink is
# what makes it visible: the membership hands the partition back to the node still holding the value
# nobody has written to since, so the stale count after the shrink is the clear down, measured.
#
# The fault is the update and the heal is the update back. The assertions are asked of the nodes
# themselves over Run Command, because what a resize moved is a question about a *store* and a read
# through the load balancer is answered by whichever copy has it.
#
#   CHAOS_RESIZE   how long a resized group is given to reach the new shape   1200 seconds

source "$(dirname "$0")/harness.sh"

banner "The tier grows to nine" "Growing loses nothing: a node fills itself in before it joins."

setup
seed

resize=${CHAOS_RESIZE:-1200}

# The precondition this experiment measures against, and the shape the heal has to put back.
expect "$(shape)" '[6,3,[2]]' "the tier is six nodes in three zones of two"

before=$(instances asyncdb | cut -f1)

inject()
{
	echo "  Updating $stack to nine database instances."

	stack_update Nodes=9
}

# Six again, and it is a fault of its own: the group chooses which instance of a zone to terminate,
# so the pair a zone is left with is not necessarily the pair it started as.
heal()
{
	stack_update Nodes=6
}

preflight()
{
	may_resize Nodes=9

	# The invariants are asked of the stores, so this one needs the agent as well as the update.
	may_run $(instances asyncdb | cut -f1)
}

fault_start || { verdict; exit 1; }

# Three zones of three. The count is asserted per zone and not only in total, because six nodes in
# three zones and nine nodes in three zones are the same predicate to anything that counts zones,
# and a group that put all three new instances in one subnet would pass it.
await '(.nodes | length) == 9 and (.zones | length) == 3 and ([ .zones[] | length ] | unique) == [3]' \
	"$resize" "the tier is nine nodes in three zones of three"

# The assertion the growth is worth making. Every zone's split was redrawn, so a third of the
# keyspace changed hands inside each of them — and none of it moved to a node that did not already
# hold it, because the rebuild runs before the node registers.
expect_readable 60 5 "every seeded record can still be read once the tier has grown"
printf '  ---- reads at nine nodes: %s\n' "$(codes)"

expect_writes 20 "every write is taken by three copies split three ways"
expect "$(scan_status)" 200 "a scan is answered by a zone of three"

# The growth, asked of the stores rather than through the load balancer. Every zone holds the
# keyspace between its three nodes, and no key is in two of them: the node that gained a partition
# fetched it, and the two that lost part of theirs let it go.
expect_copies "at nine nodes"

# Reported and not asserted: node-stops is the test of the rebuild, and this is the same mechanism
# arriving for a different reason. An instance that cannot be asked says nothing here.
after=$(instances asyncdb | cut -f1)

for joined in $(comm -13 <(echo "$before") <(echo "$after")); do
	printf '  ---- %s says: %s\n' "$joined" \
		"$(ssm_run "$joined" "$(container_logs) | grep -i rebuil | tail -1")"
done

# What a resize has to move besides the records. Nothing but a lease takes a claim away, so a node
# that joins a tier whose 256 claims are all held leads nothing at all until the nodes the wider
# membership stopped naming give theirs up — and no read would ever show it, because only a write
# needs a leader. It is asked of the joined nodes themselves: `leads` is a node's own count, and the
# load balancer answers from whichever node it picked.
for joined in $(comm -13 <(echo "$before") <(echo "$after")); do
	await_node "$joined" '.leads > 0' "$settle" "$joined leads partitions of its own once it has joined"
done

# Written while the tier is nine wide, so it lands on the owners the wider membership names. What
# the shrink does to it is the point of the second half.
grown=grown-$RANDOM

expect "$(write_seed "$grown")" 0 "every seeded key takes a write at nine nodes"

fault_stop

await '(.nodes | length) == 6 and (.zones | length) == 3 and ([ .zones[] | length ] | unique) == [2]' \
	"$resize" "the tier is six nodes in three zones of two again"

# The shrink through the load balancer, and the sharpest thing this experiment says. A key the
# membership hands back to a node that stopped owning it answers with whatever that node held when
# it stopped — the seed, and not the write above — so **a stale answer here is a copy that was never
# cleared down**. There is no third possibility: either the node let the key go and has to be given
# it again, or it kept it and is a version behind.
#
# Gone is not asserted on. A key every zone's terminated instance was the only holder of went with
# them, and no mechanism in the cluster puts that back — every copy of it left at once.
read -r same older absent < <(held 60 "$grown")

printf '  ---- of 60 seeded keys after the shrink: %s fresh, %s stale, %s gone\n' \
	"$same" "$older" "$absent"

expect "$older" 0 "no key answers a value that stopped being written to before the tier grew"

# A key the shrink moved to a node that never held it is a 404, and a cluster that cannot answer is
# not — a count alone cannot tell them apart, and this is the one that would be this experiment
# hiding a real fault.
expect_codes '^(2|404)' "every read of the shrunken tier is answered or refused as not found"

expect_writes 20 "every write is taken once the tier is six again"
expect_round_trip 10 "a key written after the shrink reads back what was written"

# And the same two invariants on the way down. A node that has been handed a partition back holds
# it, whether it kept it or had to be given it again, and no node of a zone holds a key another
# node of that zone owns.
expect_copies "once the tier is six again"

verdict
