#! /usr/bin/env bash

# The chaos suite. Every experiment in this folder, in an order chosen so that what each one
# leaves behind is something the next one can still be run against.

source "$(dirname "$0")/harness.sh"

here=$(dirname "$0")

default="node-stops zone-lost nodes-go-deaf etcd-unreachable node-latency disk-fills"
default="$default containers-restart write-storm nodes-added nodes-removed zone-retired"
default="$default etcd-quorum-lost"

experiments=${CHAOS_EXPERIMENTS:-$default}

setup

whole()
{
	health_matches \
		'(.nodes | length) == 6 and (.zones | length) == 3 and (.write_stalled | not) and (.incomplete | not)' 6
}

preflight_chaos || exit 1

if ! whole; then
	echo "The cluster is not whole, so there is nothing worth breaking:" >&2
	curl --fail --silent --max-time 10 "$base/health" | jq . >&2
	exit 1
fi

if ! every_node_whole; then
	echo "A node is holding less than it owns, so nothing here would be measuring its own fault." >&2
	exit 1
fi

echo "Six nodes in three zones, nothing stalled, every node holding what it owns. Starting."

seed

passed=0
skipped=0
failed=0
summary=

for name in $experiments; do
	script=$here/$name.sh

	if [ ! -x "$script" ]; then
		echo "No experiment named $name." >&2
		failed=$((failed + 1))
		summary="$summary\nMISSING $name"
		continue
	fi

	echo "::group::$name"
	"$script"
	status=$?
	echo "::endgroup::"

	case $status in
		0)
			passed=$((passed + 1))
			summary="$summary\nPASS    $name"
			;;
		77)
			skipped=$((skipped + 1))
			summary="$summary\nSKIP    $name"
			continue
			;;
		*)
			failed=$((failed + 1))
			summary="$summary\nFAIL    $name"
			;;
	esac

	if ! whole; then
		echo

		# A stack that was deleted underneath the run is not a cluster that failed to recover,
		# and the difference is the whole diagnosis. It happens: the pipeline tears its stack
		# down whether the tests passed or not.
		if ! curl --fail --silent --max-time 10 "$base/health" > "$work/last"; then
			echo "$base stopped answering during $name. The stack is gone, or going." >&2
			summary="$summary\nGONE    the stack stopped answering during $name"
			failed=$((failed + 1))
			break
		fi

		echo "The cluster did not come back after $name. Nothing after it would mean anything." >&2
		jq . < "$work/last" >&2
		summary="$summary\nSTOPPED after $name"
		failed=$((failed + 1))
		break
	fi

	if ! every_node_whole; then
		echo
		echo "A node came back from $name holding less than it owns." >&2
		summary="$summary\nSHORT   after $name"
		failed=$((failed + 1))
		break
	fi
done

echo
echo "Experiments"
printf '%b\n' "$summary" | sed '/^$/d;s/^/ /'
printf '\n %s passed, %s failed, %s skipped.\n' "$passed" "$failed" "$skipped"

[ "$failed" = 0 ]
