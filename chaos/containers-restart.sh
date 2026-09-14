#! /usr/bin/env bash

# Every container is killed at once, several times over, while a client is writing.
#
#   doc/runbook/nodes.md#the-container-has-stopped
#   doc/runbook/index.md#what-recovers-by-itself
#   doc/runbook/rebuild.md#when-it-runs
#
# The database process on every instance is killed with SIGKILL at the same moment, and then again,
# and again, because a node killed while it is opening the store the last kill left it is the case
# one clean crash never reaches.
#
# The kill is SIGKILL from the host rather than `docker stop`, deliberately. A node stopped
# cleanly revokes its lease and leaves the membership before it stops accepting, which is
# doc/runbook/nodes.md#shutting-a-node-down-deliberately and the gentle half of this. What is
# under test is the process that was given no chance to say anything: no revoke, no flush, and a
# RocksDB nobody closed.
#
#   CHAOS_RESTART_ROUNDS   how many times every container is killed      3
#   CHAOS_RESTART_SETTLE   how long the cluster is given between them    20 seconds

source "$(dirname "$0")/harness.sh"

banner "Every container is killed" "The store outlives the process, and what it held is still there."

setup
seed
start_load

rounds=${CHAOS_RESTART_ROUNDS:-3}
between=${CHAOS_RESTART_SETTLE:-20}

# 256 partitions, cluster::partition_of. Every claim in the cluster is held on the claiming node's
# membership lease, so killing every node at once frees every one of them.
partitions=256

ids=$(instances asyncdb | cut -f1)
count=$(echo "$ids" | grep -c .)

expect "$count" 6 "the stack is running six database nodes"
[ "$count" = 6 ] || { verdict; exit 1; }

# A value of this run's own, so that a record read afterwards says which write it came from rather
# than only that something is there.
stamp=$RANDOM
before=before-$stamp

container=$chaos_container

# A table and a key of the probe's own, so that the terms it drives up on one node never reach a
# write the load or the seed made: a copy that has seen a high term refuses everything older for
# that partition, which a real write ordered in a smaller term is. It is created through the load
# balancer like any table and dropped at the end, so it perturbs no assertion here and is gone
# before the later experiments walk the seeded table.
probe_table=$table-term-probe
probe_key=probe

# The two terms are far above any etcd revision a short run reaches, and the low one is what a
# leader superseded by the high one would carry. The node under test is the first of the six.
probe_high=2000000000
probe_low=1000000000
probe_node=$(echo "$ids" | head -1)

# term_write <node> <term> <value> — the status a forwarded write carrying that term gets from the
# node's own curl. This is exactly the request a copy receives from the leader that ordered a
# write: X-Asyncdb-Forwarded so it is served where it lands, and X-Asyncdb-Term so it is gated by
# cluster::etcd_cluster::accept rather than ordered afresh. Sent over Run Command because the API
# port is not the load balancer's, so only the instance itself can reach it.
term_write()
{
	ssm_run "$1" "curl -s -o /dev/null -w '%{http_code}' --max-time 10 -X PUT \
		-H '$forwarded_header: true' -H '$term_header: $2' \
		-H 'Content-Type: application/octet-stream' --data '$3' \
		'http://localhost:8080/table/$probe_table/key/$probe_key'"
}

# term_read <node> — what that node holds for the probe key in its own store, forwarded so it
# answers out of its own copy and asks the membership nothing.
term_read()
{
	ssm_run "$1" "curl -s --max-time 10 -H '$forwarded_header: true' \
		'http://localhost:8080/table/$probe_table/key/$probe_key'"
}

# term_probe_arm — before the kills, establish a term on one node and prove the guard is live:
# a write carrying a term older than the newest that node has applied is refused (stale_leader,
# 409). The high term goes into that node's own store as the probe value.
term_probe_arm()
{
	local created armed rejected

	created=$(status --request PUT --header 'Content-Type: application/json' \
		--data '{}' "$base/table/$probe_table")

	case $created in
		200 | 201) ;;
		*) die "Could not create $probe_table: $created." ;;
	esac

	armed=$(term_write "$probe_node" "$probe_high" high)
	expect "$armed" 204 "a forwarded write in a high term is applied on $probe_node"

	rejected=$(term_write "$probe_node" "$probe_low" stale-before)
	expect "$rejected" 409 "and one in an older term is refused before the restart"
}

# term_probe_check — after the restart, the same older-term write, which a node that persisted the
# term it applied would refuse again. This one forgot it (cluster::etcd_cluster::terms is in memory
# only) and accepts it. The assertion is the behaviour that should hold, so it FAILS on the current
# code deliberately — README.md#the-one-assertion-here-that-is-meant-to-fail is why.
term_probe_check()
{
	local accepted held

	accepted=$(term_write "$probe_node" "$probe_low" stale-after)
	expect "$accepted" 409 "the older-term write is still refused after the restart"

	held=$(term_read "$probe_node")

	if [ "$held" = high ]; then
		result 0 "and the higher-term value it held was not overwritten by the older one"
	else
		result 1 "and the higher-term value it held was not overwritten by the older one — holds \"$held\""
	fi
}

# expect_seed_replicated <when> — every seeded record is in every zone's stores, and no key is in
# two stores of one zone. It is given time the way expect_copies is: a key written to the node
# standing in for one that was away is one the returning owner has to be handed, and until it is,
# that key is in two stores of its zone.
expect_seed_replicated()
{
	local deadline=$((SECONDS + converge)) zone missing short duplicates i

	for (( i = 0; i < records; i++ )); do
		echo "$i"
	done | sort -u > "$work/replicated"

	while :; do
		short=

		collect_holdings
		duplicates=$(zone_duplicates)

		for zone in $(zones_held); do
			cat "$work/holdings/$zone."* 2> /dev/null | sort -u > "$work/held.$zone"

			missing=$(comm -23 "$work/replicated" "$work/held.$zone" | grep -c .)

			[ "$missing" = 0 ] || short="$short $zone($missing)"
		done

		[ -n "$short" ] || [ -n "$duplicates" ] || break
		[ "$holdings_asked" = "$holdings_total" ] || break
		[ "$SECONDS" -lt "$deadline" ] || break

		sleep 15
	done

	expect "$holdings_asked" "$holdings_total" "every node said what it holds $1"

	if [ -z "$short" ]; then
		result 0 "every seeded record is in every zone $1"
	else
		result 1 "every seeded record is in every zone $1 — short:$short"
	fi

	if [ -z "$duplicates" ]; then
		result 0 "no key is held by two nodes of a zone $1"
	else
		result 1 "no key is held by two nodes of a zone $1 — held twice: $duplicates"
	fi
}

# await_cluster <timeout> — the two things only a node can be asked, waited for: the claims it
# holds and whether it is short of what it owns. **A membership is not a cluster that can be
# written to** — a write is ordered by the leader of the key's partition.
await_cluster()
{
	local deadline=$((SECONDS + $1)) id led=0 short=

	while :; do
		led=0
		short=

		# shellcheck disable=SC2086
		ssm_all 'curl -s --max-time 5 http://localhost:8080/health' $ids

		for id in $ids; do
			# A node that could not be asked is counted as one that is short, the way
			# every_node_whole counts it: a silent answer must not read like a whole node.
			if [ -s "$work/answer.$id" ] \
				&& jq --exit-status 'has("incomplete")' < "$work/answer.$id" > /dev/null 2>&1; then
				led=$((led + $(jq -r '.leads // 0' < "$work/answer.$id")))

				jq --exit-status '.incomplete | not' < "$work/answer.$id" > /dev/null 2>&1 \
					|| short="$short $id"
			else
				short="$short $id"
			fi
		done

		[ "$led" != "$partitions" ] || [ -n "$short" ] || break
		[ "$SECONDS" -lt "$deadline" ] || break

		sleep 10
	done

	expect "$led" "$partitions" "every partition is led again"

	if [ -z "$short" ]; then
		result 0 "no node came back holding less than it owns"
	else
		result 1 "no node came back holding less than it owns — $short"
	fi

	printf '  ---- the %s of them lead %s partitions between them\n' "$count" "$led"
}

inject()
{
	local round id

	: > "$work/rounds"

	for (( round = 1; round <= rounds; round++ )); do
		echo "  Round $round of $rounds: killing the container on all $count nodes at once."

		# shellcheck disable=SC2086
		kill_containers $ids || return 1

		for id in $ids; do
			printf '%s %s\n' "$id" "$(tr '\n' ' ' < "$work/answer.$id")" >> "$work/rounds"
		done

		tail -n "$count" "$work/rounds" | sed "s/^/  ---- round $round: /"

		# A kill of a node that is not ordering writes again is a kill with nothing in flight to
		# lose. The claims a round frees are back a lease and a claim pass later, which is what
		# this waits out before taking them away again.
		[ "$round" = "$rounds" ] || sleep "$between"
	done
}

# Nothing stands to be taken away: the restart policy is what brings a container back, and every
# round waits for it. This is the case it did not — a container still down when the assertions
# ended would leave every experiment after this one a node short.
heal()
{
	# shellcheck disable=SC2086
	container_start $ids
}

preflight()
{
	# shellcheck disable=SC2086
	may_run $ids

	# Run Command has no dry run, so the other half of "would this work?" is whether the container
	# this would kill is one the instance can name. Listing them applies nothing.
	local answered

	answered=$(ssm_run "$(echo "$ids" | head -1)" "docker ps --format '{{.Image}}' | grep -c asyncdb")

	expect "${answered:-0}" 1 "one container of the asyncdb image is running on the first node"
}

# Written before the kills and never again, so that a record read afterwards is either this value
# or a fault.
failed=$(write_seed "$before")

expect "$failed" 0 "every seeded record was written before the first kill"

# Raised on one node just before the kills, so the window in which its heightened term could refuse
# a load write to the same partition is a round trip and no more — and a write so refused is one the
# cluster never acknowledged, which no assertion here rests on.
term_probe_arm

started=$SECONDS

fault_start || { verdict; exit 1; }

load_report "while the containers were being killed"

back=$(grep -c 'answering again' "$work/rounds")

expect "$back" "$((rounds * count))" "every container came back by itself after every kill"

# The fault is over as soon as the last container is back, so this is the safety net for one that
# is not — and every assertion below it is the recovery.
fault_stop

await '(.nodes | length) == 6 and (.zones | length) == 3' "$settle" \
	"every node registered again after the last kill"

await_cluster "$settle"

await_writes 20 "$settle" "every copy takes a write again"

logs="docker logs --since $((SECONDS - started))s $container 2>&1 | grep -ci rebuil || true"

# shellcheck disable=SC2086
if ssm_all "$logs" $ids; then
	rebuilt=$(cat "$work"/answer.* | awk '{ total += $1 } END { print total + 0 }')

	expect "$rebuilt" 0 "no node rebuilt itself, because the store it opened is the one it wrote"
else
	result 1 "every node said whether it rebuilt itself"
fi

# What was at rest rather than in flight, which is the seed. Presence is asked with attempts,
# because a read is answered by one copy; the value is asked without them, because a 2xx carrying
# the wrong value is never the load balancer's doing.
absent=$(readable "$records" 3)

expect "$absent" 0 "every seeded record is still there"

read -r same other gone <<< "$(held 60 "$before")"

expect "$other" 0 "and none of them came back holding an older value"

printf '  ---- of 60 seeded records: %s hold the value written before the kills,' "$same"
printf ' %s an older one, %s no copy answered for\n' "$other" "$gone"

# The keys the write and round trip checks leave behind in the seeded table carry a stamp of their
# own apiece, so they are written once too.
copies_agree "$table" "once every container had come back"

expect_seed_replicated "once every container had been killed $rounds times"

expect_round_trip 10 "a key written after the last kill reads back what was written"

expect_load_kept "once every container had come back"

# The node is back on its own store, so the term it applied before the kills is the thing the
# restart is asked to have kept.
term_probe_check

status --request DELETE "$base/table/$probe_table" > /dev/null

verdict
