#! /usr/bin/env bash

# The chaos suite. Every experiment in this folder, in an order chosen so that what each one
# leaves behind is something the next one can still be run against.
#
#   chaos/run.sh
#   CHAOS_SSM=1 chaos/run.sh                              with the faults that need the agent
#   CHAOS_EXPERIMENTS='zone-lost node-stops' chaos/run.sh  a subset, in the order given
#
# This is what the pipeline runs, after the load tests and before the stack is torn down. It
# fails the build the way perf/ does: an experiment whose assertions did not hold is a non-zero
# exit, and the summary at the end says which.

source "$(dirname "$0")/harness.sh"

here=$(dirname "$0")

# Agentless first, because they need nothing of the instances themselves and so run anywhere.
# The SSM tier is skipped unless CHAOS_SSM is set — see chaos/README.md for why. And etcd losing
# quorum is last, because it is the only one that leaves the cluster having been wrong about
# itself rather than merely short of a node.
default="node-stops zone-lost scan-loses-a-node etcd-unreachable node-latency disk-fills etcd-quorum-lost"

experiments=${CHAOS_EXPERIMENTS:-$default}

setup

# Chaos against a cluster that is already broken proves nothing at all, so the first thing is
# to establish that there was something to break.
whole()
{
	health_matches '(.nodes | length) == 6 and (.zones | length) == 3 and (.write_stalled | not)' 6
}

preflight_fis || exit 1

if whole; then
	echo "Six nodes in three zones, nothing stalled. Starting."
else
	echo "The cluster is not whole, so there is nothing worth breaking:" >&2
	curl --fail --silent --max-time 10 "$base/health" | jq . >&2
	exit 1
fi

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

	# Every experiment ends by asserting that what it broke came back, so a cluster that is not
	# whole here is one that did not recover — and every experiment after this would be
	# measuring that rather than its own fault. Stop, and say so.
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
done

echo
echo "Experiments"
printf '%b\n' "$summary" | sed '/^$/d;s/^/ /'
printf '\n %s passed, %s failed, %s skipped.\n' "$passed" "$failed" "$skipped"

[ "$failed" = 0 ]
