#! /usr/bin/env bash

# A scan fails while everything else works.
#
#   doc/runbook/nodes.md#a-scan-fails-while-everything-else-works
#
# One database node in each of the three zones stops answering on its API port, while its
# process carries on renewing its lease. That is the one state in which a scan fails and a key
# read does not: a scan is asked of one zone and every node of that zone has to answer, so a
# zone with a node that does not answer is a zone to give up on — and when every zone has one,
# there is no zone left to give up to.
#
# It is also the state that is next to impossible to arrange by hand, which is the argument for
# doing this with FIS at all: three targets, one instance each, filtered by availability zone.
#
# The fault goes in through the SSM agent. See the note on the SSM faults in chaos/README.md.

source "$(dirname "$0")/harness.sh"

banner "One node in every zone goes deaf" "Key reads carry on. Scans have nowhere left to fall back to."

setup
seed

# One instance per zone, which is what makes this the scan failure rather than a zone failure.
deaf=$(instances asyncdb | awk '!seen[$2]++ { print $1 }')
count=$(echo "$deaf" | wc -l)

expect "$count" 3 "one node was picked in each of three zones"
[ "$count" = 3 ] || { verdict; exit 1; }

echo "  Blackholing port 8080 on $(echo "$deaf" | tr '\n' ' ')"

seconds=${CHAOS_DEAF_SECONDS:-300}

cat > "$work/template.json" <<EOF
{
	"description": "asyncdb chaos: one node in every zone stops answering",
	"roleArn": "$role",
	"stopConditions": [ { "source": "none" } ],
	"tags": { "Name": "asyncdb-chaos" },
	"targets": {
		"Deaf": {
			"resourceType": "aws:ec2:instance",
			"resourceArns": $(arns instance $deaf),
			"selectionMode": "ALL"
		}
	},
	"actions": {
		"blackhole": {
			"actionId": "aws:ssm:send-command",
			"parameters": {
				"duration": "PT$((seconds / 60))M",
				"documentArn": "arn:aws:ssm:$region::document/AWSFIS-Run-Network-Blackhole-Port",
				"documentParameters": "{\"Port\":\"8080\",\"Protocol\":\"tcp\",\"TrafficType\":\"ingress\",\"DurationSeconds\":\"$seconds\",\"InstallDependencies\":\"True\"}"
			},
			"targets": { "Instances": "Deaf" }
		}
	}
}
EOF

fis_start "$work/template.json" || { verdict; exit 1; }
fis_await_running || { verdict; exit 1; }

# The whole difficulty of this failure mode: the node is not answering and is still a member,
# because renewing a lease says nothing about being able to serve a request.
holds '(.nodes | length) == 6 and (.zones | length) == 3' 30 \
	"a node that does not answer stays in the membership while its lease is renewed"

# A read needs one copy and passes over a node that does not answer, so every key still reads.
expect_reads 40 "every key read is answered by a copy that does answer"

# A scan is not that. Every zone is short a node, and there is no fourth zone.
scan=$(scan_status)
expect_not "$scan" 200 "a scan fails when every zone has a node that does not answer"

case $scan in
	5*) result 0 "the scan fails with a 5xx and not a refusal — $scan" ;;
	*) result 1 "the scan fails with a 5xx and not a refusal — $scan" ;;
esac

# Writes are reported and not asserted on. The runbook says reads and writes of individual keys
# are fine here, and for reads that is exactly true — but a write needs *every* copy, and a key
# whose copies include one of these three nodes is a key that cannot be written. Roughly seven
# writes in eight touch one, so this number is expected to be large.
printf '  ---- %s of 20 writes were refused while three nodes were deaf\n' "$(write_check 20)"

echo "  Waiting for the fault to be removed."

fis_await_end $((seconds + 300)) > /dev/null

await '(.nodes | length) == 6 and (.zones | length) == 3' "$settle" \
	"every node is answering again"

expect "$(scan_status)" 200 "the scan is answered once a zone is whole"
expect_writes 20 "every write is taken once every copy answers"

verdict
