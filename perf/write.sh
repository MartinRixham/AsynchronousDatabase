#! /usr/bin/env bash

source "$(dirname "$0")/harness.sh"

table=${TABLE:-perf_load}

value_bytes=${VALUE_BYTES:-1024}
content_type=application/octet-stream

status()
{
	curl --silent --output /dev/null --write-out '%{http_code}' "$@" || true
}

setup()
{
	head -c "$value_bytes" /dev/urandom | base64 | tr -d '\n' > "$work/value"
	truncate --size "$value_bytes" "$work/value"

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

	for (( i = 0; i < requests; i++ )); do
		request "$base/table/$table/key/$((worker * requests + i))"
	done
}

setup

echo "PUT $base/table/$table/key, $value_bytes byte values, $threads threads," \
	"$requests requests each." >&2

verdict=0

measure || verdict=$?

echo
echo "Table"
curl --silent "$base/table/$table" | sed 's/^/ /'
echo

exit $verdict
