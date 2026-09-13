#! /usr/bin/env bash

# Every experiment's preflight, and no fault at all.

source "$(dirname "$0")/harness.sh"

here=$(dirname "$0")

export CHAOS_VALIDATE=1

# A preflight breaks nothing and takes seconds, so there is nothing for a client to be doing while
# it runs. It is the one thing here that turns the load off.
export CHAOS_LOAD=0

setup
preflight_chaos || exit 1

passed=0
failed=0

all="node-stops zone-lost nodes-go-deaf etcd-unreachable node-latency disk-fills"
all="$all containers-restart write-storm nodes-added nodes-removed zone-retired etcd-quorum-lost"

for name in ${CHAOS_EXPERIMENTS:-$all}; do
	printf '\n-- %s\n' "$name"

	if "$here/$name.sh" > "$work/out" 2>&1; then
		passed=$((passed + 1))
		grep '  PASS ' "$work/out" || tail -2 "$work/out"
	else
		failed=$((failed + 1))
		sed 's/^/  /' "$work/out" | tail -12
	fi
done

printf '\n %s experiments would run, %s would not.\n' "$passed" "$failed"

[ "$failed" = 0 ]
