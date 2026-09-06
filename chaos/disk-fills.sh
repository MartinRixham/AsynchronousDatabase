#! /usr/bin/env bash

# The disk fills, and RocksDB says so.
#
#   doc/runbook/storage.md#rocksdb-returned-an-error
#   doc/runbook/storage.md#the-disk-is-filling
#   doc/runbook/storage.md#writes-are-stalled
#
# One database node's root volume is filled. It is thirty gigabytes shared with the image, the
# logs and the store, and nothing here caps or watches the size of the store — so this is the
# failure the deployment is closest to having by accident.
#
# What it tests is that a store that cannot take a write says so in the documented way: a write
# is refused with an error code a client can branch on, and never taken and lost. Reads are
# unaffected, because a read does not write and the other zones hold their copies anyway.
#
# This is the most invasive experiment in the suite — a full root volume is a docker daemon and
# an SSM agent with nowhere to write either — and it fills the whole disk, because anything short
# of the whole disk is not this failure at all. AWSFIS-Run-Disk-Fill's Percent is a percentage of
# the disk and not of what is free on it, so ninety per cent of a thirty gigabyte volume left
# three gigabytes to write into and every write was taken. Only Percent 100 fills what is free,
# and even that leaves three hundred kilobytes of slack, which is why the writes below carry a
# megabyte each. The fault goes in through the SSM agent, as the note on the SSM faults in
# chaos/README.md describes.

source "$(dirname "$0")/harness.sh"

banner "A node's disk fills" "Writes are refused with a documented code. Reads carry on."

setup
seed

full=$(instances asyncdb | cut -f1 | head -1)
seconds=${CHAOS_DISK_SECONDS:-240}
percent=${CHAOS_DISK_PERCENT:-100}

echo "  Filling the disk on $full to $percent%."

cat > "$work/template.json" <<EOF
{
	"description": "asyncdb chaos: one node's disk fills",
	"roleArn": "$role",
	"stopConditions": [ { "source": "none" } ],
	"tags": { "Name": "asyncdb-chaos" },
	"targets": {
		"Node": {
			"resourceType": "aws:ec2:instance",
			"resourceArns": $(arns instance "$full"),
			"selectionMode": "ALL"
		}
	},
	"actions": {
		"fill": {
			"actionId": "aws:ssm:send-command",
			"parameters": {
				"duration": "PT$((seconds / 60))M",
				"documentArn": "arn:aws:ssm:$region::document/AWSFIS-Run-Disk-Fill",
				"documentParameters": "{\"Percent\":\"$percent\",\"DurationSeconds\":\"$seconds\",\"InstallDependencies\":\"True\"}"
			},
			"targets": { "Instances": "Node" }
		}
	}
}
EOF

fis_start "$work/template.json" || { verdict; exit 1; }
fis_await_running || { verdict; exit 1; }

# A write needs every copy, so a write of a key this node holds a copy of is a write this node
# has to take — and it cannot. Roughly half of them, because a node holds half of its zone.
#
# Each one carries a megabyte, which is what makes it a write the disk cannot take rather than
# one that fits in the slack the fill leaves behind. It is well inside the sixteen megabyte limit
# in doc/database/reference.md, and the nodes that can take it take it.
value=$work/value
head -c $((1024 * 1024)) /dev/zero | tr '\0' 'x' > "$value"

refused=0
codes=

for i in $(seq 1 30); do
	body=$(curl --silent --max-time 20 --request PUT --data-binary "@$value" \
		--header 'Content-Type: application/octet-stream' \
		--write-out '\n%{http_code}' "$base/table/$table/key/full-$i")

	# The status is the last line of what curl wrote and the error document is everything
	# before it, so the two are told apart here rather than asked for twice.
	case ${body##*$'\n'} in
		2*) ;;
		*)
			refused=$((refused + 1))
			codes="$codes $(printf '%s' "${body%$'\n'*}" | jq -r '.error.code' 2> /dev/null)"
			;;
	esac
done

printf '  ---- %s of 30 writes were refused, with codes:%s\n' "$refused" \
	"$(echo "$codes" | tr ' ' '\n' | sort -u | tr '\n' ' ')"

if [ "$refused" = 0 ]; then
	result 1 "a node whose disk is full refuses the writes it cannot take"
else
	result 0 "a node whose disk is full refuses the writes it cannot take"
fi

# The code is what a client branches on, and there are exactly two it should ever see here:
# back pressure while compaction catches up, and the status RocksDB itself returned.
case $codes in
	*storage_error* | *write_stalled*)
		result 0 "the refusals carry a documented error code" ;;
	*)
		result 1 "the refusals carry a documented error code — saw:$codes" ;;
esac

# A read does not write, and the record is in every zone.
expect_reads 40 "every read is answered while one node cannot write"

echo "  Waiting for the fault to be removed."

fis_await_end $((seconds + 300))

# A store that stopped for want of space does not notice the space coming back by itself: the
# background error is sticky, and repository::rocksdb_repository::written is what resumes it, on
# the next write that node is given. Without that, this is a node that refuses every write to
# half the keyspace until somebody restarts it — which is not the recovery
# doc/runbook/storage.md documents.
await '(.nodes | length) == 6 and (.zones | length) == 3 and (.write_stalled | not)' "$settle" \
	"the node is whole again and nothing is stalled"

expect_writes 30 "every write is taken once there is room for it"

# And the megabytes go, now that there is a store to take the delete: the disk they would be on
# next is the one this experiment just filled.
for i in $(seq 1 30); do
	status --request DELETE "$base/table/$table/key/full-$i" > /dev/null
done

verdict
