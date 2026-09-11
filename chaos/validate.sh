#! /usr/bin/env bash

# Every experiment's preflight, and no fault at all.
#
#   chaos/validate.sh
#
# An experiment's preflight resolves the targets it would break and dry runs what it would do to
# them, so this is the whole of "would this experiment run?" for a handful of API calls each and
# nothing applied. It is seconds, where finding the same mistake by running the suite is the
# length of the experiment that hits it.
#
# It needs the stack up, because what a preflight resolves is real instances and subnets.

source "$(dirname "$0")/harness.sh"

here=$(dirname "$0")

export CHAOS_VALIDATE=1

setup
preflight_chaos || exit 1

passed=0
failed=0

all="node-stops zone-lost scan-loses-a-node etcd-unreachable node-latency disk-fills"
all="$all containers-restart nodes-added nodes-removed zone-retired etcd-quorum-lost"

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
