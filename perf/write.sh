#! /usr/bin/env bash

# Writes, over persistent connections, reported as latency percentiles.
#
#   THREADS=6 REQUESTS=5000 VALUE_BYTES=1024 perf/write.sh

source "$(dirname "$0")/harness.sh"

# A table of its own, dropped and recreated so that a run always starts on an empty one and two
# runs are comparable. Nothing else should be using this name.
#
# **It is left standing when the run ends**, because a read run is pointed at a key a write run
# wrote, so whatever runs both is what drops it. Half a gigabyte of two megabyte values left on a
# cluster is every pass of every node reading it back afterwards.
table=${TABLE:-perf_load}

# A value may be 16 MiB, and this is a kilobyte, because the request count it pairs with is
# REQUESTS=5000 on sixteen threads — eighty thousand of anything larger is a run measured in
# gigabytes. Size and count trade against each other: raise this and lower those together, which
# is what build.yaml does to load the deployed stack.
value_bytes=${VALUE_BYTES:-1024}
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
		--data "{}" "$base/table/$table")

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

	# curl asks for a 100 Continue on any body over a kilobyte, and the database never sends one:
	# only the nginx in front of it answers, on its behalf. Left in, a payload of any size worth
	# measuring spends a second of curl's own timeout per request whenever this is pointed at the
	# binary rather than at the proxy — which is a measurement of curl. src/http/http_client.cpp
	# clears the header for the same reason.
	printf 'header = "Expect:"\n'

	# A key range of its own per worker, so that workers never write the same key at the same
	# time, and so that every write of a first run is an insert rather than an overwrite.
	for (( i = 0; i < requests; i++ )); do
		request "$base/table/$table/key/$((worker * requests + i))"
	done
}

setup

echo "PUT $base/table/$table/key, $value_bytes byte values, $threads threads," \
	"$requests requests each." >&2

measure
verdict=$?

echo
echo "Table"
curl --silent "$base/table/$table" | sed 's/^/ /'
echo

# The table is reported whether the run passed or failed, so the verdict is carried past it.
exit $verdict
