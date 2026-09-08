#! /usr/bin/env bash

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
