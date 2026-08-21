#! /usr/bin/env bash

# Writes, over persistent connections, reported as latency percentiles.
#
#   THREADS=6 REQUESTS=5000 VALUE_BYTES=100 DURABILITY=wal perf/write.sh
#
# Durability is the knob worth turning: none skips the write ahead log, wal is the default, and
# sync waits for an fsync, which the wiki puts at hundreds of microseconds to milliseconds.

source "$(dirname "$0")/harness.sh"

# A table of its own, dropped and recreated so that a run always starts on an empty one and two
# runs are comparable. Nothing else should be using this name.
table=${TABLE:-perf_load}
durability=${DURABILITY:-wal}
value_bytes=${VALUE_BYTES:-100}
content_type=application/octet-stream

status()
{
	curl --silent --output /dev/null --write-out '%{http_code}' "$@"
}

setup()
{
	# Incompressible, because the table compresses with lz4 and a run of one byte would not
	# measure the compression a real value pays for.
	head -c "$value_bytes" /dev/urandom | base64 | tr -d '\n' | head -c "$value_bytes" > "$work/value"

	echo "Dropped $table ($(status --request DELETE "$base/table/$table"))." >&2

	local created
	created=$(status --request PUT --header 'Content-Type: application/json' \
		--data "{\"contentType\":\"$content_type\"}" "$base/table/$table")

	if [ "$created" != 201 ]; then
		echo "Could not create $table: $created." >&2
		exit 1
	fi
}

write_requests()
{
	local worker=$1
	local i

	printf 'request = "PUT"\n'
	printf 'data-binary = "@%s"\n' "$work/value"
	printf 'header = "Content-Type: %s"\n' "$content_type"
	printf 'header = "Durability: %s"\n' "$durability"

	# A key range of its own per worker, so that workers never write the same key at the same
	# time, and so that every write of a first run is an insert rather than an overwrite.
	for (( i = 0; i < requests; i++ )); do
		request "$base/table/$table/key/$((worker * requests + i))"
	done
}

setup

echo "PUT $base/table/$table/key, $value_bytes byte values, durability $durability," \
	"$threads threads, $requests requests each." >&2

measure

echo
echo "Table"
curl --silent "$base/table/$table" | sed 's/^/ /'
echo
