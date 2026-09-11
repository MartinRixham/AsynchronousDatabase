#! /usr/bin/env bash

# etcd loses quorum, and every database node falls back to being a cluster of one.
#
#   doc/runbook/membership.md#etcd-has-lost-quorum
#   doc/runbook/membership.md#etcd-cannot-be-reached
#
# Two of the three etcd members are stopped. The survivor answers reads and takes no writes, so
# leases stop being renewed and every membership key expires — and a database node that can read
# no membership puts itself in the list and believes it holds every key. It answers reads out of
# its own store, for keys it has never held included, and refuses every write: a membership of one
# leads nothing, and the image forbids a write nothing ordered. This is the experiment that shows
# both halves, and that the refusal is what keeps the wrong reads from becoming wrong data.
#
# It is deliberately the last experiment in the suite. What it leaves behind is a cluster that
# has been briefly wrong about itself, and the pipeline deletes the stack next.
#
# Two things keep it reversible. The instances are stopped and started rather than terminated,
# so they come back on the private addresses ASYNCDB_ETCD was given at boot — a tier that was
# replaced instead would strand every database node, which is the other half of that runbook
# page and not a thing to do to a stack something else still has to run against. And the group's
# ReplaceUnhealthy is suspended first, which is the runbook's own remedy for a tier that is
# churning: an EC2 health check finds a stopped instance unhealthy within a minute or two.

source "$(dirname "$0")/harness.sh"

banner "etcd loses quorum" "Every node becomes a cluster of one, and the membership comes back by itself."

setup
seed
start_load

etcd=$(instances etcd | cut -f1)
count=$(echo "$etcd" | wc -l)

expect "$count" 3 "the stack is running three etcd members"
[ "$count" = 3 ] || { verdict; exit 1; }

stopped=$(echo "$etcd" | head -2 | tr '\n' ' ')
group=$(aws cloudformation describe-stack-resource --stack-name "$stack" \
	--logical-resource-id EtcdAutoScalingGroup \
	--query 'StackResourceDetail.PhysicalResourceId' --output text)

inject()
{
	if aws autoscaling suspend-processes --auto-scaling-group-name "$group" \
		--scaling-processes ReplaceUnhealthy
	then
		result 0 "the group will not replace the members while they are stopped"
	else
		result 1 "the group will not replace the members while they are stopped"
		return 1
	fi

	echo "  Stopping $stopped of the group $group."

	aws ec2 stop-instances --instance-ids $stopped > /dev/null
}

# The members are waited for rather than started and left: what comes next is the assertion that
# the membership repopulated itself, and a member that is still booting is one that has not
# answered a single renewal yet.
heal()
{
	aws ec2 start-instances --instance-ids $stopped > /dev/null 2>&1

	aws ec2 wait instance-running --instance-ids $stopped 2> /dev/null

	aws autoscaling resume-processes --auto-scaling-group-name "$group" \
		--scaling-processes ReplaceUnhealthy > /dev/null 2>&1

	return 0
}

preflight()
{
	may_stop $stopped
	may_suspend "$group"
}

fault_start || { verdict; exit 1; }

# A node that cannot read a membership does not report one. That is the whole diagnosis, and it
# is what tells this apart from a node that is merely slow.
#
# It is still asked through the load balancer, and that is the one thing here that rests on the
# load balancer's own behaviour rather than on this database's: every node can order a write no
# more than any other, so every node fails its health check, and a target group with nothing
# healthy left in it is one the load balancer sends to all of them. Losing etcd altogether is a
# cluster that goes on serving what it holds, and not a cluster nothing can reach.
await '(.nodes | length) == 1' "$settle" \
	"every node fell back to a membership of one, which is itself"

# The half that would diverge the data, and does not: a cluster of one has nothing to order a
# write with, and the image sets ASYNCDB_UNLED_WRITES=false, so the write is refused rather than
# written where no leader ordered it and no copy has it.
refuse_writes 10 503 \
	"a node with no membership refuses every write with no_leader, rather than taking it alone"

# The half that is still visible to a client. A cluster of one believes it holds every key, so it
# answers for keys it has never seen rather than asking the copies that have them. This is reported
# and not asserted on: it is the documented behaviour, and how much of it a client sees is how the
# load balancer happened to spread the reads.
missed=$(read_check 60)
printf '  ---- %s of 60 reads of seeded records answered something other than 2xx\n' "$missed"

[ "$missed" -gt 0 ] \
	&& echo "  ---- an isolated node answering for keys it has never held, as doc/runbook/membership.md describes"

load_report "while etcd had no quorum"

echo "  Waiting for the members to come back."

fault_stop

# Both keys etcd holds for asyncdb are leased, so neither outlived the members that wrote them
# and neither had to. The cluster writes itself back into an empty etcd within a lease.
await '(.nodes | length) == 6 and (.zones | length) == 3' "$recovery" \
	"the membership repopulated itself once quorum was back"

await '.leads > 0' "$settle" "every node is leading partitions again"

expect_reads 60 "every read of a seeded record is answered again"
expect_writes 20 "every write is ordered and taken by every copy again"

# etcd holds the membership and the claims and never a record, so a quorum it lost cost the
# cluster the ability to order writes and nothing it had already taken.
expect_load_kept "once quorum was back"

verdict
