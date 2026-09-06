#! /usr/bin/env bash

# A node that is up but wrong.
#
#   doc/runbook/nodes.md#a-node-that-is-up-but-wrong
#   doc/runbook/nodes.md#threads-are-all-waiting
#   doc/runbook/membership.md#leadership-keeps-moving
#
# One database node is given a second or so of latency to everything in the VPC. It is slow to
# serve and slow to renew, but a renewal every three seconds against a ten second lease survives
# a delay this size — so the node stays in the membership, which is the whole difficulty of this
# failure mode. A node that is down leaves. A node that is slow does not, and every node
# forwarding to it waits the full thirty second timeout with a thread of its own.
#
# The latency is deliberately below what would cost the node its lease. Raise CHAOS_LATENCY_MS
# past the etcd client's five second timeout and this becomes
# doc/runbook/membership.md#leadership-keeps-moving instead: renewals start failing, and
# leadership moves every lease.
#
# The fault goes in through the SSM agent. See the note on the SSM faults in chaos/README.md.

source "$(dirname "$0")/harness.sh"

banner "A node is slow" "It stays in the membership, and everything forwarding to it waits."

setup
seed

slow=$(instances asyncdb | cut -f1 | head -1)
cidr=$(aws ec2 describe-vpcs --vpc-ids "$vpc" --query 'Vpcs[0].CidrBlock' --output text)

seconds=${CHAOS_SLOW_SECONDS:-240}
delay=${CHAOS_LATENCY_MS:-1200}

echo "  Adding ${delay}ms to everything $slow says to $cidr."

cat > "$work/template.json" <<EOF
{
	"description": "asyncdb chaos: one node is slow",
	"roleArn": "$role",
	"stopConditions": [ { "source": "none" } ],
	"tags": { "Name": "asyncdb-chaos" },
	"targets": {
		"Node": {
			"resourceType": "aws:ec2:instance",
			"resourceArns": $(arns instance "$slow"),
			"selectionMode": "ALL"
		}
	},
	"actions": {
		"latency": {
			"actionId": "aws:ssm:send-command",
			"parameters": {
				"duration": "PT$((seconds / 60))M",
				"documentArn": "arn:aws:ssm:$region::document/AWSFIS-Run-Network-Latency",
				"documentParameters": "{\"Interface\":\"eth0\",\"DelayMilliseconds\":\"$delay\",\"JitterMilliseconds\":\"100\",\"Sources\":\"$cidr\",\"DurationSeconds\":\"$seconds\",\"InstallDependencies\":\"True\"}"
			},
			"targets": { "Instances": "Node" }
		}
	}
}
EOF

fis_start "$work/template.json" || { verdict; exit 1; }
fis_await_running || { verdict; exit 1; }

# The assertion this experiment exists for. A lease renewed every three seconds survives a
# delay of about a second, so the cluster goes on believing this node is a copy worth sending
# keys to — and it is, slowly.
holds '(.nodes | length) == 6 and (.zones | length) == 3' 60 \
	"a node that is slow stays in the membership, because renewal says nothing about serving"

# A read goes to one copy and a slow copy is still a copy, so nothing is refused. What it costs
# is a thread on whichever node forwarded, for as long as this one takes.
expect_reads 30 "every read is answered, slowly"
expect_writes 10 "every write is taken, slowly"

# What it looks like from outside is latency and not errors, which is why the runbook's advice
# is to go and look at the node rather than to read the status codes.
printf '  ---- a read through the load balancer now takes %ss\n' \
	"$(curl --silent --output /dev/null --max-time 40 --write-out '%{time_total}' \
		"$base/table/$table/key/1")"

echo "  Waiting for the fault to be removed."

fis_await_end $((seconds + 300)) > /dev/null

await '(.nodes | length) == 6 and (.zones | length) == 3' "$settle" \
	"the membership never changed and is still six nodes in three zones"

expect_reads 30 "every read is answered once the latency is gone"

verdict
