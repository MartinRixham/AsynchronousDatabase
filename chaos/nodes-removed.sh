#! /usr/bin/env bash

# The partition factor is decreased, and then increased again.
#
#   doc/runbook/rebuild.md#when-ownership-moves     a node fetches what it has been handed
#   doc/runbook/storage.md#what-there-is-not        and nothing brings back a copy that is gone
#   doc/database/cluster.md                         a read asks the other copies
#
# The database tier goes from six instances to three by a stack update, which is three zones of one
# where it was three zones of two. **The replication factor does not move**: three zones is three
# copies whatever each zone is split between, so this is the other axis on its own, and it is the
# one that costs something.
#
# Each zone's survivor is handed the partitions the terminated node had and holds nothing for any of
# them, so it goes and fetches them. A read would find the record anyway — the other copies are
# asked for what this node has nothing of — but the zone that was handed it is a copy short until it
# does, and a partition whose other copies then go is a partition with nothing behind it. That is
# the assertion here.
#
# The zones lose different halves, because the split inside a zone is hashed over that zone's own
# node addresses, so a key is beyond fetching only where every zone lost it at once. That number is
# reported and never asserted: nothing here puts back a copy that no node has.
#
#   CHAOS_RESIZE   how long a resized group is given to reach the new shape   1200 seconds

source "$(dirname "$0")/harness.sh"

banner "The tier shrinks to three" "One node a zone, each holding the whole of its zone's copy."

setup
seed

resize=${CHAOS_RESIZE:-1200}

expect "$(shape)" '[6,3,[2]]' "the tier is six nodes in three zones of two"

inject()
{
	echo "  Updating $stack to three database instances."

	stack_update Nodes=3
}

heal()
{
	stack_update Nodes=6
}

preflight()
{
	may_resize Nodes=3
	may_run $(instances asyncdb | cut -f1)
}

fault_start || { verdict; exit 1; }

await '(.nodes | length) == 3 and (.zones | length) == 3 and ([ .zones[] | length ] | unique) == [1]' \
	"$resize" "the tier is three nodes in three zones of one"

# The claim worth separating from the loss below. Halving the nodes did not halve the copies: a
# write still needs three of them and a read still has three to choose from.
expect_writes 20 "every write is still taken by three copies, one node to a zone"
expect_round_trip 10 "a key written after the shrink reads back what was written"
expect "$(scan_status)" 200 "a scan is answered by a zone of one"

# The assertion the shrink is worth making, asked of the stores themselves. Every zone is down to
# one node, so that node holds the whole of its zone's copy — anything else is a partition the zone
# has been handed and has not fetched, which is a copy that exists only in the zones that happen
# still to have it. The other half of expect_copies has nothing to say at one node a zone: a key
# cannot be in two stores of a zone that has one.
expect_copies "at three nodes"

# What a shrink costs, in numbers. A key is gone only where every zone lost it, which is why this
# is a fraction rather than half of the seed — and it is reported and never asserted, because the
# number is the deployment's own hashing rather than anything the database decides.
absent=$(readable 60 3)

printf '  ---- of 60 seeded keys at three nodes: %s are held by no copy\n' "$absent"

# This is the assertion, and it is about the shape of the failure rather than the size of it. A key
# no copy holds is a 404. Anything else is a cluster that cannot answer, which is a different fault
# and would be this one hiding it.
expect_codes '^(2|404)' "a key a shrink took away is refused as not found and not as an error"

fault_stop

await '(.nodes | length) == 6 and (.zones | length) == 3 and ([ .zones[] | length ] | unique) == [2]' \
	"$resize" "the tier is six nodes in three zones of two again"

# The node that joins each zone rebuilds from another zone before it registers, and the node it
# joins has to let go of what it handed over. What neither of them can do is bring back a key every
# zone lost, which is why the number is printed here as well: the growth is not a repair.
absent=$(readable 60 3)

printf '  ---- of 60 seeded keys once the tier is six again: %s are held by no copy\n' "$absent"

expect_writes 20 "every write is taken once the tier is six again"
expect_round_trip 10 "a key written once the tier is six again reads back what was written"

expect_copies "once the tier is six again"

verdict
