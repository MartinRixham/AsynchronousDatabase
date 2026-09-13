#! /usr/bin/env bash

# The partition factor is decreased in two zones, and then increased again.
#
#   doc/runbook/rebuild.md#when-ownership-moves     a node fetches what it has been handed
#   doc/database/cluster.md                         a read asks the other copies
#
# The database tier goes from six instances to four by a stack update, which the group balances into
# one zone of two and two zones of one. **The replication factor does not move, and neither does any
# key's last copy**: the zone that keeps both of its nodes holds the whole keyspace throughout, so a
# shrink of two takes no record away, and every read is answered 2xx from the first to the last.
#
# Each shrunk zone's survivor is handed the partitions the terminated node had and holds nothing for
# any of them until it has fetched them, so a read that reaches it has to find the record elsewhere.
#
#   CHAOS_RESIZE   how long a resized group is given to reach the new shape   1200 seconds

source "$(dirname "$0")/harness.sh"

banner "The tier shrinks by two" "Two zones down to one node, the third keeping both of its own."

setup
seed
start_load

resize=${CHAOS_RESIZE:-1200}

expect "$(shape)" '[6,3,[2]]' "the tier is six nodes in three zones of two"

inject()
{
	echo "  Updating $stack to four database instances."

	stack_update Nodes=4
}

heal()
{
	stack_update Nodes=6
}

preflight()
{
	may_resize Nodes=4
	may_run $(instances asyncdb | cut -f1)
}

fault_start || { verdict; exit 1; }

await '(.nodes | length) == 4 and (.zones | length) == 3 and ([ .zones[] | length ] | sort) == [1,1,2]' \
	"$resize" "the tier is four nodes, one zone of two and two zones of one"

expect_writes 20 "every write is still taken by three copies"
expect_round_trip 10 "a key written after the shrink reads back what was written"
expect "$(scan_status)" 200 "a scan is answered by a copy that is left"

expect_copies "at four nodes"
expect_reads 60 "every seeded key is read at the first attempt at four nodes"

expect_load_reads "every read the load made while the tier shrank to four was answered 2xx"
load_report "while the tier was four"

fault_stop

await '(.nodes | length) == 6 and (.zones | length) == 3 and ([ .zones[] | length ] | unique) == [2]' \
	"$resize" "the tier is six nodes in three zones of two again"

expect_reads 60 "every seeded key is read at the first attempt once the tier is six again"
expect_writes 20 "every write is taken once the tier is six again"
expect_round_trip 10 "a key written once the tier is six again reads back what was written"

expect_copies "once the tier is six again"

stop_load
expect_load_reads "every read the load made while the tier grew back to six was answered 2xx"
report_load_kept "while the tier was four and then six again"

verdict
