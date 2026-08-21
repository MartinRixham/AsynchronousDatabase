#! /usr/bin/env bash

set -u

# Job control, so that each worker becomes its own process group and stop() can signal the
# curl inside it. Without it a stopped run leaves its workers orphaned and still loading.
set -m

# Times are read a digit at a time below, and sort -n has to agree about the decimal point.
export LC_ALL=C

threads=${THREADS:-16}
requests=${REQUESTS:-5000}
base=${BASE:-http://localhost:8080/asyncdb}

work=$(mktemp -d)
workers=()
count=0

stop()
{
	local worker

	for worker in ${workers[@]+"${workers[@]}"}; do
		kill -- "-$worker" 2> /dev/null
	done

	rm -rf "$work"
}

trap stop INT TERM EXIT

# A curl config file is its command line in a file, and it pairs each url with the output that
# follows it. Options that are not a url, such as the method and the body, apply to all of them.
request()
{
	printf 'url = "%s"\noutput = "/dev/null"\n' "$1"
}

# %{num_connects} is 0 for a reused connection, so the sum is the number of connections opened.
# A transfer that fails reports status 000 and curl carries on to the next url.
worker()
{
	echo "Starting thread $1." >&2

	curl --silent --show-error --config "$work/requests.$1" \
		--write-out '%{http_code} %{time_total} %{num_connects}\n' \
		> "$work/out.$1" 2> "$work/err.$1"
}

run()
{
	local i

	for (( i = 0; i < threads; i++ )); do
		worker "$i" &
		workers+=("$!")
	done

	wait
}

# Counting the distinct values first keeps this a handful of iterations rather than one per request.
connections()
{
	local total=0 lines connects

	while read -r lines connects; do
		total=$((total + lines * connects))
	done < <(cut -d ' ' -f 3 "$work/all" | sort -n | uniq -c)

	echo "$total"
}

# Bash has no floating point, so shift the decimal point of curl's seconds rather than divide.
# Two decimal places of a millisecond is 10 microseconds, so the six curl writes round to five.
milliseconds()
{
	local seconds=${1%%.*}
	local fraction=0

	[[ $1 == *.* ]] && fraction=${1#*.}00000

	fraction=$(((10#${fraction:0:6} + 5) / 10))

	printf '%d.%02d' "$((seconds * 1000 + fraction / 100))" "$((fraction % 100))"
}

# The times are sorted, so a percentile is a line number: 0 is the fastest, 100 the slowest.
latency()
{
	local index=$(((count * $2 + 50) / 100))

	[ "$index" -lt 1 ] && index=1

	printf ' %-4s %9s ms\n' "$1" "$(milliseconds "$(sed -n "${index}p;${index}q" "$work/times")")"
}

report()
{
	local elapsed=$1

	# A run can finish inside a millisecond, and nothing below divides by zero.
	[ "$elapsed" -gt 0 ] || elapsed=1

	cat "$work"/out.* > "$work/all"
	cat "$work"/err.* > "$work/errors"

	cut -d ' ' -f 2 "$work/all" | sort -n > "$work/times"

	count=$(wc -l < "$work/times")

	printf '\n%s responses in %d.%02ds over %s connections\n\n' \
		"$count" "$((elapsed / 1000))" "$((elapsed % 1000 / 10))" "$(connections)"

	echo "Status"
	cut -d ' ' -f 1 "$work/all" | sort | uniq -c | sed 's/^/ /'

	if [ -s "$work/errors" ]; then
		echo
		echo "Errors"
		sort "$work/errors" | uniq -c | sort -rn | sed 's/^/ /'
	fi

	[ "$count" -gt 0 ] || return

	echo
	echo "Latency"

	latency min 0
	latency p50 50
	latency p90 90
	latency p99 99
	latency max 100

	printf '\n %s requests per second\n' "$((count * 1000 / elapsed))"
}

measure()
{
	local i start

	for (( i = 0; i < threads; i++ )); do
		write_requests "$i" > "$work/requests.$i"
	done

	start=$(date +%s%N)

	run

	# Milliseconds, so that the report is integer arithmetic all the way down.
	report "$((($(date +%s%N) - start) / 1000000))"
}
