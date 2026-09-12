#! /usr/bin/env bash

# A node has failed, and the instance behind it is replaced.
#
#   doc/runbook/nodes.md#a-node-does-not-answer
#   doc/runbook/nodes.md#an-instance-was-replaced
#   doc/runbook/rebuild.md
#
# One database instance is stopped. It stops renewing its lease, so it leaves the membership
# within one of them; the auto scaling group's EC2 health check then finds an instance that is
# stopped, terminates it and launches another — which is a new root volume, an empty database,
# and the one path in the system that fills itself in. This is the experiment worth running if
# only one is: it is the only test anywhere of the rebuild, and the rebuild is the only thing
# that puts a lost copy back.

source "$(dirname "$0")/harness.sh"

banner "A node is stopped" "Reads survive it, writes recover with the membership, the replacement rebuilds."

setup
seed
start_load

before=$(instances asyncdb | cut -f1)
count=$(echo "$before" | wc -l)

expect "$count" 6 "the stack is running six database nodes"
[ "$count" = 6 ] || { verdict; exit 1; }

victim=$(echo "$before" | head -1)

# Stopping is the whole fault, and what happens to the instance afterwards belongs to the group:
# its health check is EC2, so a stopped instance is an unhealthy one that it terminates and
# replaces, which is the point of the experiment. Starting it again on a timer of our own would
# race that — for as long as both exist the group is at seven instances, which is a zone with
# three nodes in it and the next experiment's precondition broken.
inject()
{
	echo "  Stopping $victim."

	aws ec2 stop-instances --instance-ids "$victim" > /dev/null
}

# The group replaces a stopped instance, so the instance this stopped is one that may already be
# gone by the time anything here reads it. Starting it again is best effort and for the case the
# group did not: a stack left five nodes short is worse than a run that took longer.
heal()
{
	local stopped

	stopped=$(aws ec2 describe-instances --instance-ids "$victim" \
		--query 'Reservations[].Instances[].State.Name' --output text 2> /dev/null)

	[ "$stopped" = stopped ] && aws ec2 start-instances --instance-ids "$victim" > /dev/null 2>&1

	return 0
}

preflight()
{
	may_stop "$victim"
}

fault_start || { verdict; exit 1; }

# The lease is what removes it, and the load balancer's health check is what stops routing to
# it. Five nodes in three zones is both of those having happened: the zone it was in still has
# its other node, so the number of copies never changed.
await '(.nodes | length) == 5 and (.zones | length) == 3' "$settle" \
	"the stopped node left the membership and the zone count did not change"

load_report "while the node was going away"

# Once the membership has settled the cluster is whole again as far as a client is concerned:
# a read needs one copy and every key still has three, and a write needs every copy of a
# membership that no longer names the node that is gone.
expect_readable 40 5 "every seeded record can still be read with the node out of the membership"
printf '  ---- reads with the node gone: %s\n' "$(codes)"
expect_writes 20 "every write is taken with the node out of the membership"
expect "$(scan_status)" 200 "a scan is answered by a copy of the partition that is there"

echo "  Waiting for the group to replace it."

await '(.nodes | length) == 6 and (.zones | length) == 3' "$recovery" \
	"the group replaced the instance and it rejoined"

# Whatever came back is whatever is not in the list from before the fault.
after=$(instances asyncdb | cut -f1)
replacement=$(comm -13 <(echo "$before") <(echo "$after") | head -1)

if [ -z "$replacement" ]; then
	result 1 "a replacement instance was launched"
else
	result 0 "a replacement instance was launched — $replacement"

	# The rebuild is on the DEBUG log, which the image has on, and it runs before the node
	# registers — so a node that is in the membership at all has already finished one.
	rebuilt=$(ssm_run "$replacement" "$(container_logs) | grep -i rebuil | tail -5")

	echo "  $replacement says: ${rebuilt:-nothing about a rebuild}"

	case $rebuilt in
		*Rebuilt*) result 0 "the replacement rebuilt itself from another zone before joining" ;;
		*) result 1 "the replacement rebuilt itself from another zone before joining" ;;
	esac

	# A rebuild that ran is a store that holds the schema. Asking the node itself is the only
	# way to tell it from a read the other zones answered on its behalf.
	tables=$(ssm_run "$replacement" 'curl -s --max-time 5 http://localhost:8080/table')

	case $tables in
		*"\"$table\""*) result 0 "the replacement holds the table in its own store" ;;
		*) result 1 "the replacement holds the table in its own store — it answers $tables" ;;
	esac
fi

# The replacement rebuilds what it will own, and what it will own is not what the instance it
# replaced owned: the hash is over node addresses and the replacement has a new one, so the split
# inside that zone is redrawn. A key that moved to the *other* node of the zone is one that node
# never held and no rebuild fills in, because its store was never empty — that one arrives on the
# reconcile pass the membership change starts, which is a pass or two later than this. So what is
# asked here is whether the record can be read, which is true throughout: a node asks the other
# copies for what it holds nothing of.
expect_readable 40 5 "every seeded record can still be read once the replacement has joined"
printf '  ---- reads once the replacement joined: %s\n' "$(codes)"
expect_writes 20 "every write is taken once the replacement has joined"

# The fault ended when the group terminated the instance, which is what the assertions above
# waited for. This is the case it did not: an instance still sitting stopped is started again.
fault_stop

# The instance that went took a copy of everything it held with it, and the replacement rebuilt
# from another zone. A write that was acknowledged had been taken by all three copies, so losing
# one of them loses nothing.
expect_load_kept "once the replacement had joined"

verdict
