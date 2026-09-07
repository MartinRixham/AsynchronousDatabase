#! /usr/bin/env bash

# The disk fills, and the node refuses the writes it cannot take.
#
#   doc/runbook/storage.md#rocksdb-returned-an-error
#   doc/runbook/storage.md#the-disk-is-filling
#   doc/runbook/storage.md#writes-are-stalled
#
# One database node's root volume is filled. It is thirty gigabytes shared with the image, the
# logs and the store, and nothing here caps or watches the size of the store — so this is the
# failure the deployment is closest to having by accident.
#
# What it tests is that a node whose disk is full refuses the writes it cannot take, in a way a
# client can branch on, and never takes one and loses it. Reads are unaffected, because a read
# does not write and the other zones hold their copies anyway.
#
# The layer that refuses is not always the database, and that is worth knowing rather than
# asserting around. nginx spools a request body larger than client_body_buffer_size — 8 KiB, and
# nothing here sets it — to a temporary file, so on a full disk it answers `pwrite() ... (28: No
# space left on device)` with a 500 out of 50x.json and the database is never asked at all. That
# is what a client writing a real value sees, and it is why the writes below come in two sizes:
# a megabyte, which the proxy refuses, and a kilobyte, which is small enough to reach the store.
#
# This is the most invasive experiment in the suite — a full root volume is a docker daemon and
# an SSM agent with nowhere to write either — and it fills the whole disk, because anything short
# of the whole disk is not this failure at all. AWSFIS-Run-Disk-Fill's Percent is a percentage of
# the disk and not of what is free on it, so ninety per cent of a thirty gigabyte volume left
# three gigabytes to write into and every write was taken. Only Percent 100 fills what is free.
# The fault goes in through the SSM agent, as the note on the SSM faults in chaos/README.md
# describes.

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

# write_refusals <body file> <key prefix> — the number of thirty writes that were refused, with
# every refusal's code and status kept in $work/refusals and every key that was taken kept in
# $work/taken. A count says an assertion failed and only the codes say how, which is the job
# read_check does in harness.sh. The status is kept beside the code because it is what says which
# layer refused: a 500 carrying `unavailable` is the proxy and a 500 carrying `storage_error` is
# the store, and telling the two apart is the whole of what this experiment got wrong before — it
# asserted the store's codes against writes the store was never given.
write_refusals()
{
	local i body code refused=0

	: > "$work/refusals"

	for i in $(seq 1 30); do
		body=$(curl --silent --max-time 20 --request PUT --data-binary "@$1" \
			--header 'Content-Type: application/octet-stream' \
			--write-out '\n%{http_code}' "$base/table/$table/key/$2-$i")

		# The status is the last line of what curl wrote and the error document is
		# everything before it, so the two are told apart here rather than asked for twice.
		case ${body##*$'\n'} in
			2*)
				echo "$2-$i" >> "$work/taken"
				;;
			*)
				refused=$((refused + 1))
				code=$(printf '%s' "${body%$'\n'*}" | jq -r '.error.code' 2> /dev/null)
				printf '%s(%s)\n' "${code:-no-code}" "${body##*$'\n'}" >> "$work/refusals"
				;;
		esac
	done

	echo "$refused"
}

refusals()
{
	sort "$work/refusals" | uniq -c | sort -rn | awk '{ printf "%s×%s ", $1, $2 }'
}

# The code is what a client branches on, and three of them are documented for this. Which one
# arrives says which layer refused the write, and any of the three is a client that knows what
# happened and can retry it:
#
#   unavailable    nginx's, out of 50x.json, when it could not spool the body onto the full disk
#   storage_error  RocksDB's, when the store itself could not take the write
#   write_stalled  RocksDB's, when it is back pressure rather than a failure
#
# Anything else is what this asserts against: an empty code is a body that is not one of ours —
# nginx's own 413 page, or nothing at all behind a 000 — and a client cannot branch on it.
documented()
{
	local undocumented
	undocumented=$(grep -cvE '^(unavailable|storage_error|write_stalled)\(' "$work/refusals")

	if [ "$undocumented" = 0 ]; then
		result 0 "$1"
	else
		result 1 "$1 — $undocumented of them did not: $(refusals)"
	fi
}

fis_start "$work/template.json" || { verdict; exit 1; }
fis_await_running || { verdict; exit 1; }

: > "$work/taken"

# A write needs every copy, so a write of a key this node holds a copy of is a write this node
# has to take — and it cannot. Roughly half of them, because a node holds half of its zone.
#
# A megabyte is well inside the sixteen megabyte limit in doc/database/reference.md and far
# outside what nginx will hold in memory, so what refuses these is the proxy on whichever node
# the load balancer picked, which is the full one about one time in six.
spooled=$work/spooled
head -c $((1024 * 1024)) /dev/zero | tr '\0' 'x' > "$spooled"

refused=$(write_refusals "$spooled" full)

printf '  ---- %s of 30 megabyte writes were refused: %s\n' "$refused" "$(refusals)"

if [ "$refused" = 0 ]; then
	result 1 "a node whose disk is full refuses the writes it cannot take"
else
	result 0 "a node whose disk is full refuses the writes it cannot take"
fi

documented "the refusals carry a documented error code"

# A kilobyte fits in the buffer nginx holds in memory, so these are the writes that reach the
# store at all — and what they say is reported rather than asserted on. RocksDB preallocates the
# write ahead log, tens of megabytes reserved when the store opened and long before the disk
# filled, so a full volume is a store that carries on taking writes into space it already holds;
# `IO error: No space left on device` arrives when that runs out or a flush needs room for an
# SST. How much of it is left after a fault of this length is the deployment's timing and not the
# database's behaviour, which is why there is nothing here to assert.
inline=$work/inline
head -c 1024 /dev/zero | tr '\0' 'x' > "$inline"

refused=$(write_refusals "$inline" small)

printf '  ---- %s of 30 kilobyte writes, which are the ones that reach the store, were refused: %s\n' \
	"$refused" "$(refusals)"

if [ "$refused" != 0 ]; then
	documented "the store's own refusals carry a documented error code"
fi

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

# Never taken and lost, which is the half of this that matters most and the half no error code
# can say. A write that answered 2xx was taken by every copy, so every one of these keys is still
# there now the volume has room again — and a few attempts each, because a read goes to one copy
# and the load balancer picks which node is asked.
missing=0
taken=$(wc -l < "$work/taken")

while read -r key; do
	found=0

	for attempt in 1 2 3; do
		case $(status "$base/table/$table/key/$key") in
			2*) found=1; break ;;
		esac
	done

	[ "$found" = 1 ] || missing=$((missing + 1))
done < "$work/taken"

if [ "$missing" = 0 ]; then
	result 0 "every write that was taken while the disk was full is still there"
else
	result 1 "every write that was taken while the disk was full is still there — $missing of $taken are gone"
fi

expect_writes 30 "every write is taken once there is room for it"

# And the megabytes go, now that there is a store to take the delete: the disk they would be on
# next is the one this experiment just filled.
for i in $(seq 1 30); do
	status --request DELETE "$base/table/$table/key/full-$i" > /dev/null
	status --request DELETE "$base/table/$table/key/small-$i" > /dev/null
done

verdict
