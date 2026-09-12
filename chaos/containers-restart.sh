#! /usr/bin/env bash

# Every container is killed at once, several times over, while a client is writing.
#
#   doc/runbook/nodes.md#the-container-has-stopped
#   doc/runbook/index.md#what-recovers-by-itself
#   doc/runbook/rebuild.md#when-it-runs
#
# The database process on every instance is killed with SIGKILL at the same moment, and nothing
# here starts it again: `docker run --restart always` is what brings the container back, which is
# the runbook's claim rather than this experiment's doing. Then it is done again, and again,
# because a node killed while it is opening the store the last kill left it is the case one clean
# crash never reaches.
#
# **It is the one experiment here that is allowed to cost nothing at all.** A container is a
# process and the store is a volume: `/var/lib/asyncdb` outlives the container, so a node comes
# back at the address it already had, owning the partitions it already owned and holding what it
# already held. Nothing is replaced, nothing is rebuilt and no record changes hands — so every
# number after the kills is the number before them, and that is what makes the fault worth its
# severity. Six nodes killed three times over is a cluster that lost nothing, or it is a bug.
#
# What it asserts, in the order of what losing it would mean:
#
#   every write the cluster acknowledged reads back what was written   nothing taken is lost
#   every seeded record is there, at the value written before          nothing at rest is lost
#   no copy of a key answers a different value from another copy       nothing diverged
#   every seeded record is in every zone, and in one store of it       nothing moved
#   every partition is led and no node holds less than it owns         it is a cluster again
#
# The first of those is the load the harness keeps on every experiment here, and this is the fault
# it was written for: a write is answered only once every copy has taken it, and every copy of it
# is then killed with the write ahead log unflushed and RocksDB never closed.
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

# The kill, and what the container is, are `container_kill_script` and `chaos_container` in
# chaos/harness.sh: write-storm kills containers too, and a second copy of a fault is a second
# fault to keep true.
container=$chaos_container

# expect_seed_replicated <when> — every seeded record is in every zone's stores, and no key is in
# two stores of one zone. It is given time the way expect_copies is: a key written to the node
# standing in for one that was away is one the returning owner has to be handed, and until it is,
# that key is in two stores of its zone.
#
# **It is deliberately not expect_copies**, which asks that the zones hold the *same* keys. The
# recovery assertions above this one retry writes until they are taken, which is what await_writes
# is for — and a write that was refused on the way there may still have been taken by one copy,
# which leaves the zones holding different keys with nothing wrong. What is asked instead is that
# the records written while the cluster was whole are all still in every zone, which is the claim
# a kill has to leave standing.
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
# written to** — a write is ordered by the leader of the key's partition — and a kill takes every
# claim in the cluster with it, because a claim is held on the killed node's own lease. So the
# claims adding up again is the recovery this fault has to be asked about, and `leads` is a node's
# own count, which the load balancer cannot be asked for.
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

# The kills go out in one Run Command rather than six, because six sent in turn are six kills a
# wait apart — a rolling restart, which is a fault the cluster is built to ride out one node at a
# time and not the one this asserts on.
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

started=$SECONDS

fault_start || { verdict; exit 1; }

load_report "while the containers were being killed"

# The runbook's claim, and the whole of what restarts a container here. Nothing in this experiment
# starts one: a node that came back is `--restart always` doing it.
back=$(grep -c 'answering again' "$work/rounds")

expect "$back" "$((rounds * count))" "every container came back by itself after every kill"

# The fault is over as soon as the last container is back, so this is the safety net for one that
# is not — and every assertion below it is the recovery.
fault_stop

await '(.nodes | length) == 6 and (.zones | length) == 3' "$settle" \
	"every node registered again after the last kill"

await_cluster "$settle"

await_writes 20 "$settle" "every copy takes a write again"

# A restart is not a rebuild. **An empty store is the only thing that triggers one**, so a node
# whose volume outlived its container reads nothing from anybody — and a rebuild in the log since
# the kills began would say a node came back to an empty directory, which is the one way a kill
# here could cost a copy.
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

# Asked of the seeded table as well as of the load, because every key in it is written once too:
# the seeded records carry the value written above and never another, and the keys the write and
# round trip checks leave behind carry a stamp of their own apiece. So a node holding something
# else for one of them is a copy that took a different write and is waiting on no later one. What
# the value *is* was asserted above; this is that the copies say one thing.
copies_agree "$table" "once every container had come back"

# Nothing moved, because the membership came back naming the same six addresses and every node
# owns what it owned. A zone short of a seeded record is a store that did not outlive its
# container; a key in two stores of one zone is a node that came back believing it owns something
# it does not.
expect_seed_replicated "once every container had been killed $rounds times"

expect_round_trip 10 "a key written after the last kill reads back what was written"

# And the claim the whole load exists to make, over every write the cluster acknowledged while its
# every copy was being killed with the write ahead log unflushed and RocksDB never closed.
expect_load_kept "once every container had come back"

verdict
