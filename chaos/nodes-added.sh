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
#   CHAOS_RESIZE   how long a resized group is given to reach the new shape   1200 seconds

source "$(dirname "$0")/harness.sh"

banner "The tier grows to nine" "Growing loses nothing: a node fills itself in before it joins."

setup
seed
start_load

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
expect "$(scan_status)" 200 "a scan is answered by a copy of the partition"

expect_copies "at nine nodes"

# Reported and not asserted. An instance that cannot be asked says nothing here.
after=$(instances asyncdb | cut -f1)

for joined in $(comm -13 <(echo "$before") <(echo "$after")); do
	printf '  ---- %s says: %s\n' "$joined" \
		"$(ssm_run "$joined" "$(container_logs) | grep -i rebuil | tail -1")"
done

for joined in $(comm -13 <(echo "$before") <(echo "$after")); do
	await_node "$joined" '.leads > 0' "$settle" "$joined leads partitions of its own once it has joined"
done

# The codes are printed whether it passed or not, because a write that was only taken on the second
# ask is a write the cluster did not take at once and nothing else here would say so.
grown=grown-$RANDOM

await_seed "$grown" "$settle" "every seeded key takes a write at nine nodes"
printf '  ---- writes of the seed at nine nodes: %s\n' "$(codes)"

load_report "while the tier was nine"

fault_stop

await '(.nodes | length) == 6 and (.zones | length) == 3 and ([ .zones[] | length ] | unique) == [2]' \
	"$resize" "the tier is six nodes in three zones of two again"

read -r same older absent < <(held 60 "$grown")

printf '  ---- of 60 seeded keys after the shrink: %s fresh, %s stale, %s gone\n' \
	"$same" "$older" "$absent"

expect "$older" 0 "no key answers a value that stopped being written to before the tier grew"

expect_codes '^(2|404)' "every read of the shrunken tier is answered or refused as not found"

expect_writes 20 "every write is taken once the tier is six again"
expect_round_trip 10 "a key written after the shrink reads back what was written"

expect_copies "once the tier is six again"

report_load_kept "while the tier was nine and then six again"

verdict
