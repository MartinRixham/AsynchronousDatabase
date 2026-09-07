#! /usr/bin/env bash

# etcd loses quorum, and every database node falls back to being a cluster of one.
#
#   doc/runbook/membership.md#etcd-has-lost-quorum
#   doc/runbook/membership.md#etcd-cannot-be-reached
#
# Two of the three etcd members are stopped. The survivor answers reads and takes no writes, so
# leases stop being renewed and every membership key expires — and a database node that can read
# no membership puts itself in the list, holds every key, and accepts every write locally with
# no leader and no copies. The runbook calls that the one situation here that can silently
# diverge the data, and this is the experiment that shows it happening.
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

etcd=$(instances etcd | cut -f1)
count=$(echo "$etcd" | wc -l)

expect "$count" 3 "the stack is running three etcd members"
[ "$count" = 3 ] || { verdict; exit 1; }

stopped=$(echo "$etcd" | head -2 | tr '\n' ' ')
group=$(aws cloudformation describe-stack-resource --stack-name "$stack" \
	--logical-resource-id EtcdAutoScalingGroup \
	--query 'StackResourceDetail.PhysicalResourceId' --output text)

echo "  Stopping $stopped of the group $group."

duration=${CHAOS_ETCD_DURATION:-PT4M}

cat > "$work/template.json" <<EOF
{
	"description": "asyncdb chaos: etcd loses quorum",
	"roleArn": "$role",
	"stopConditions": [ { "source": "none" } ],
	"tags": { "Name": "asyncdb-chaos" },
	"targets": {
		"Members": {
			"resourceType": "aws:ec2:instance",
			"resourceArns": $(arns instance $stopped),
			"selectionMode": "ALL"
		}
	},
	"actions": {
		"stop": {
			"actionId": "aws:ec2:stop-instances",
			"parameters": {
				"startInstancesAfterDuration": "$duration",
				"completeIfInstancesTerminated": "true"
			},
			"targets": { "Instances": "Members" }
		}
	}
}
EOF

restore()
{
	aws ec2 start-instances --instance-ids $stopped > /dev/null 2>&1

	aws autoscaling resume-processes --auto-scaling-group-name "$group" \
		--scaling-processes ReplaceUnhealthy > /dev/null 2>&1

	return 0
}

cleanup_hook=restore

aws autoscaling suspend-processes --auto-scaling-group-name "$group" \
	--scaling-processes ReplaceUnhealthy \
	&& result 0 "the group will not replace the members while they are stopped" \
	|| result 1 "the group will not replace the members while they are stopped"

fis_start "$work/template.json" || { verdict; exit 1; }
fis_await_running || { verdict; exit 1; }

# A node that cannot read a membership does not report one. That is the whole diagnosis, and it
# is what tells this apart from a node that is merely slow.
await '(.nodes | length) == 1' "$settle" \
	"every node fell back to a membership of one, which is itself"

# The dangerous half, and the reason the runbook says to take an isolated node out of service:
# a cluster of one has nothing to order a write with, so it takes the write.
expect "$(write_check 10)" 0 \
	"a node with no membership accepts every write locally, with no leader and no copies"

# And the visible half. A cluster of one believes it holds every key, so it answers for keys it
# has never seen rather than asking the copies that have them. This is reported and not asserted
# on: it is the documented behaviour, and how much of it a client sees is how the load balancer
# happened to spread the reads.
missed=$(read_check 60)
printf '  ---- %s of 60 reads of seeded records answered something other than 2xx\n' "$missed"

[ "$missed" -gt 0 ] \
	&& echo "  ---- that is the silent divergence doc/runbook/membership.md warns about"

echo "  Waiting for the members to come back."

# fis_await_end and not fis_stop_now, which is the one place in the suite that still pays for a
# fault it is no longer watching, and deliberately. What starts these members again is the
# action's own startInstancesAfterDuration, and whether stopping the experiment early honours it
# or leaves two etcd instances stopped is not a thing to find out on the last experiment of a run
# against a stack the pipeline deletes next. CHAOS_ETCD_DURATION is therefore a real duration
# here rather than a ceiling, and shortening it is what makes this one cheaper.
fis_await_end $((recovery / 2))

# Both keys etcd holds for asyncdb are leased, so neither outlived the members that wrote them
# and neither had to. The cluster writes itself back into an empty etcd within a lease.
await '(.nodes | length) == 6 and (.zones | length) == 3' "$recovery" \
	"the membership repopulated itself once quorum was back"

await '.leads > 0' "$settle" "every node is leading partitions again"

expect_reads 60 "every read of a seeded record is answered again"
expect_writes 20 "every write is ordered and taken by every copy again"

verdict
