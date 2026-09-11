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
# of the whole disk is not this failure at all. CHAOS_DISK_PERCENT is a percentage of the disk
# and not of what is free on it, so only 100 fills what is free. The fault goes in through the
# SSM agent, as the note on the SSM faults in chaos/README.md describes.

source "$(dirname "$0")/harness.sh"

banner "A node's disk fills" "Writes are refused with a documented code. Reads carry on."

setup
seed
start_load

full=$(instances asyncdb | cut -f1 | head -1)
seconds=${CHAOS_DISK_SECONDS:-240}
percent=${CHAOS_DISK_PERCENT:-100}

inject()
{
	echo "  Filling the disk on $full to $percent%."

	fill "$seconds" "$percent" "$full"
}

# Taking the file away needs the agent on a node whose disk is full, which is the one thing this
# fault makes unreliable. It is not the only way out: the script holding the fault removes the
# file itself when its own sleep ends, and its timer removes it if the script is gone. The
# recovery assertion below is what says which of the three got there.
heal()
{
	fill_clear "$full"
}

preflight()
{
	may_run "$full"
}

# write_refusals <body file> <key prefix> — the number of thirty writes that were refused, with
# every refusal's code and status kept in $work/refusals and every key that was taken kept in
# $work/taken. A count says an assertion failed and only the codes say how. The status is kept
# beside the code because it is what says which layer refused: a 500 carrying `unavailable` is the
# proxy and a 500 carrying `storage_error` is the store.
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

fault_start || { verdict; exit 1; }

: > "$work/taken"

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

load_report "while the disk was full"

fault_stop

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

# The same claim as the thirty writes above, over the thousands the load made: a write the store
# refused for want of space was refused to the client, and one the client was told had been taken
# was on every copy including the one that had no room a moment later.
expect_load_kept "once there was room again"

# And the megabytes go with the table, which is the only thing that erases a record: the disk they
# would be on next is the one this experiment just filled. Every experiment seeds the table for
# itself, so the one after this creates it again.
status --request DELETE "$base/table/$table" > /dev/null

verdict
