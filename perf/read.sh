#! /usr/bin/env bash

# Reads, over persistent connections, reported as latency percentiles.
#
#   THREADS=6 REQUESTS=5000 URL=http://localhost:8080/asyncdb/table perf/load.sh

source "$(dirname "$0")/harness.sh"

url=${URL:-$base/table}

write_requests()
{
	local i

	for (( i = 0; i < requests; i++ )); do
		request "$url"
	done
}

echo "GET $url, $threads threads, $requests requests each." >&2

measure
