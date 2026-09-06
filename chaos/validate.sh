#! /usr/bin/env bash

# Every experiment's template, created and deleted again, and nothing started.
#
#   chaos/validate.sh
#
# The service checks every action, parameter and target arn in a template when it is created, so
# this is the whole of "would this experiment run?" for the price of two API calls each and no
# fault at all. It is seconds, where finding the same mistake by running the suite is the length
# of the experiment that hits it.
#
# It needs the stack up, because the arns in a template name real instances and subnets.

source "$(dirname "$0")/harness.sh"

here=$(dirname "$0")

export CHAOS_VALIDATE=1

setup
preflight_fis || exit 1

passed=0
failed=0

for name in ${CHAOS_EXPERIMENTS:-node-stops zone-lost scan-loses-a-node etcd-unreachable node-latency disk-fills etcd-quorum-lost}; do
	printf '\n-- %s\n' "$name"

	if "$here/$name.sh" > "$work/out" 2>&1; then
		passed=$((passed + 1))
		grep '  PASS the experiment template' "$work/out" || tail -2 "$work/out"
	else
		failed=$((failed + 1))
		sed 's/^/  /' "$work/out" | tail -12
	fi
done

printf '\n %s templates accepted, %s refused.\n' "$passed" "$failed"

[ "$failed" = 0 ]
