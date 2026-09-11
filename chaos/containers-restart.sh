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
#   every acknowledged key is in every zone, and in one store of it    nothing moved
#   every partition is led and no node holds less than it owns         it is a cluster again
#
# What it does not assert is that the zones hold the *same* keys, which is what the resizes ask
# and what this fault is allowed to break: a write refused to the client that a copy took anyway
# leaves a key in one zone and not another, and nothing in the cluster puts the rest of it back.
# See expect_replicated below.
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

# The base image runs containers of its own, so the one to kill is the one whose image says
# asyncdb, as container_logs in chaos/harness.sh finds it.
container='$(docker ps -a --format "{{.ID}} {{.Image}}" | awk "/asyncdb/{print \$1; exit}")'

# The container's own init, killed from the host, which is the one way to make this a crash. A
# signal sent from inside the container cannot kill its PID 1 — the kernel drops what a namespace
# sends its own init unless the init handles it — and a `docker kill` is a stop the daemon was
# asked for, which it may record as manual and a restart policy does not act on. Killing the
# process from the host depends on neither: the container exits the way it would have if the
# process had died on its own, which is the failure the runbook describes.
#
# It then waits for the node to answer on its own port, which is later than the container being
# back: a node listens only once it has opened the store and joined, so a round that waits for
# this is a round that kills a node that was serving.
kill_script="id=$container"'
[ -n "$id" ] || { echo "no asyncdb container"; exit 1; }

pid=$(docker inspect --format "{{.State.Pid}}" "$id")

[ "${pid:-0}" -gt 1 ] || { echo "no process to kill"; exit 1; }

kill -9 "$pid"

for i in $(seq 30); do
	sleep 2

	code=$(curl -s -o /dev/null -m 5 -w "%{http_code}" http://localhost:8080/health)

	[ "$code" = 000 ] || { echo "answering again after $((i * 2))s"; exit 0; }
done

echo "never answered again"
exit 1'

# survived — three numbers over the writes the cluster acknowledged: how many there were, how many
# of those keys no copy answers for, and how many answer something other than what was written.
#
# A few attempts each, because a read is answered by one copy and the load balancer picks which
# node is asked.
survived()
{
	local i attempt key code taken=0 lost=0 wrong=0

	: > "$work/codes"

	while read -r i; do
		key=probe-$stamp-$i
		taken=$((taken + 1))

		for attempt in 1 2 3; do
			code=$(read_value "$key")
			echo "$code" >> "$work/codes"

			case $code in
				2*) break ;;
			esac
		done

		case $code in
			2*) [ "$(cat "$work/value")" = "$stamp-$i" ] || wrong=$((wrong + 1)) ;;
			*) lost=$((lost + 1)) ;;
		esac
	done < "$work/acknowledged"

	printf '%s %s %s\n' "$taken" "$lost" "$wrong"
}

# copies_agree <value> <how many keys> — every node asked what it holds for those keys in its own
# store, and no two of them may answer differently. It is the question expect_copies cannot put: a
# key set says which records a zone has and never what is in them, and two copies of one key
# holding two values is a cluster that is wrong rather than one that is short.
#
# A read carrying the forwarded header is answered where it lands, so what comes back is that
# node's own store rather than the copy its zone would have found. A node that does not hold the
# key answers 404, which is every node but the three copies of it and is not a disagreement.
copies_agree()
{
	local expected=$1 keys script id key value held=0 differ=0

	keys=$(seq 0 $(( $2 - 1 )) | paste -sd' ')

	script="for key in $keys; do
	code=\$(curl -s -o /tmp/asyncdb-chaos.value -m 10 -H '$forwarded_header: true' \\
		-w '%{http_code}' http://localhost:8080/table/$table/key/\$key)

	if [ \"\$code\" = 200 ]; then
		echo \"\$key \$(cat /tmp/asyncdb-chaos.value)\"
	else
		echo \"\$key -\"
	fi
done"

	# shellcheck disable=SC2086
	if ! ssm_all "$script" $ids; then
		result 1 "every node said what it holds for a key of its own"

		return 1
	fi

	: > "$work/disagreed"

	for id in $ids; do
		while read -r key value; do
			[ "$value" = - ] && continue

			held=$((held + 1))

			[ "$value" = "$expected" ] \
				|| { differ=$((differ + 1)); printf '%s %s %s\n' "$id" "$key" "$value" >> "$work/disagreed"; }
		done < "$work/answer.$id"
	done

	if [ "$differ" = 0 ]; then
		result 0 "no copy of a key disagrees with another about what is in it"
	else
		result 1 "no copy of a key disagrees with another about what is in it — $differ of them do:"
		sed 's/^/       /' "$work/disagreed" | head -3
	fi

	printf '  ---- %s copies of %s keys answered out of their own stores\n' "$held" "$2"
}

# expect_replicated <when> — every key the cluster acknowledged is in every zone's stores, and no
# key is in two stores of one zone. It is given time the way expect_copies is: a key written to the
# node standing in for one that was away is one the returning owner has to be handed, and until it
# is, that key is in two stores of its zone.
#
# **It is deliberately not expect_copies, and the difference is the writer.** The resizes assert
# that every zone holds *the same keys*, which holds there because every write they made was
# answered 2xx. A fault that kills a leader mid-write leaves the other half of that: a write the
# client was told had failed, which a copy took anyway — and nothing in the cluster puts the other
# copies of it back. There is no read repair and no anti-entropy, and a reconcile pass moves the
# records whose owner moved, which after this fault is none of them. So the zones genuinely do not
# hold the same keys afterwards, and the keys they differ by are exactly the writes that were
# refused. Measured against a two zone cluster killed twice over: twenty-seven keys apart, every
# one of them a refused write, and no fewer three minutes later.
#
# What is asserted instead is the claim that does hold, and it is the stronger one for this fault:
# a write that was **acknowledged** is on every copy, so it is in every zone — and it stays there,
# because the only thing that erases a record is the clear down, which deletes what this zone's
# owner has confirmed holding.
expect_replicated()
{
	local deadline=$((SECONDS + converge)) zone missing short duplicates elsewhere=0 i

	{
		for (( i = 0; i < records; i++ )); do
			echo "$i"
		done

		sed "s/^/probe-$stamp-/" "$work/acknowledged"
	} | sort -u > "$work/replicated"

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
		result 0 "every key the cluster acknowledged is in every zone $1"
	else
		result 1 "every key the cluster acknowledged is in every zone $1 — short:$short"
	fi

	if [ -z "$duplicates" ]; then
		result 0 "no key is held by two nodes of a zone $1"
	else
		result 1 "no key is held by two nodes of a zone $1 — held twice: $duplicates"
	fi

	# The other side of it, reported and never asserted on: a key some zone holds and another does
	# not is a write the client was told had failed, taken by a copy anyway. Nothing here puts the
	# rest of its copies back, which is what doc/runbook/index.md means by a copy that missed a
	# write never recovering by itself.
	for zone in $(zones_held); do
		cat "$work/held.$zone"
	done | sort -u > "$work/held.all"

	for zone in $(zones_held); do
		elsewhere=$((elsewhere + $(comm -23 "$work/held.all" "$work/held.$zone" | grep -c .)))
	done

	printf '  ---- %s keys are in one zone and not another %s, every one a write that was refused\n' \
		"$elsewhere" "$1"
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
		ssm_all "$kill_script" $ids || return 1

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
	local script

	script="id=$container"'
[ -n "$id" ] || exit 0

docker start "$id" > /dev/null 2>&1 || true'

	# shellcheck disable=SC2086
	ssm_all "$script" $ids > /dev/null 2>&1

	return 0
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

# Both probes start before the fault rather than after it: what this experiment breaks it breaks
# inside inject, and a probe started afterwards would have watched the recovery alone.
start_probe
start_writes "$stamp"

fault_start || { verdict; exit 1; }

stop_probe
stop_writes

# The writes the cluster said it had taken, which is what two of the checks below are about. The
# last line the writer got to may be half a line, so a line that is not a number and a status is
# not one of them.
awk '$2 ~ /^2[0-9][0-9]$/ { print $1 }' "$work/written" > "$work/acknowledged"

probe_report "while the containers were being killed"

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

# The one thing no error code can say, and the reason the writer ran at all. A write that answered
# 2xx was taken by every copy of the key, and every copy of it was then killed with the write
# ahead log unflushed and RocksDB never closed.
read -r taken lost wrong <<< "$(survived)"

expect_not "$taken" 0 "the cluster took writes while it was being killed"
expect "$lost" 0 "every write the cluster took while it was being killed is still there"
expect "$wrong" 0 "and every one of them reads back what was written"

printf '  ---- %s writes were acknowledged across %s rounds of kills, %s lost, %s changed\n' \
	"$taken" "$rounds" "$lost" "$wrong"

# And what was at rest rather than in flight. Presence is asked with attempts, because a read is
# answered by one copy; the value is asked without them, because a 2xx carrying the wrong value is
# never the load balancer's doing.
absent=$(readable "$records" 3)

expect "$absent" 0 "every seeded record is still there"

read -r same other gone <<< "$(held 60 "$before")"

expect "$other" 0 "and none of them came back holding an older value"

printf '  ---- of 60 seeded records: %s hold the value written before the kills,' "$same"
printf ' %s an older one, %s no copy answered for\n' "$other" "$gone"

copies_agree "$before" 8

# Nothing moved, because the membership came back naming the same six addresses and every node
# owns what it owned. A zone short of a key it acknowledged is a store that did not outlive its
# container; a key in two stores of one zone is a node that came back believing it owns something
# it does not.
expect_replicated "once every container had been killed $rounds times"

expect_round_trip 10 "a key written after the last kill reads back what was written"

verdict
