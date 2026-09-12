#! /usr/bin/env bash

# Faults that overlap, under a client that never gives up on a write.
#
#   doc/runbook/index.md#what-recovers-by-itself
#   doc/runbook/nodes.md#the-container-has-stopped
#   doc/runbook/nodes.md#an-instance-was-replaced
#   doc/runbook/membership.md#the-membership-is-wrong
#   doc/runbook/rebuild.md#when-ownership-moves
#
# Every other experiment here applies one fault and asks what it cost. This one applies them on
# top of each other — a zone cut off while another zone's containers are killed under it, an
# instance stopped while a third zone's container is killed, and then every zone's pair of
# containers killed at once in turn, twice through — and asks the question none of them asks:
# **whether a cluster that was never left alone can be driven back into agreeing with itself.**
#
# What makes that question answerable is the client, and it is written the way it is for one
# reason. A write is **retried until the cluster takes it**, however long that is and however many
# times it is refused, so every write it ever issued was acknowledged — which is what entitles the
# assertions to ask about all of them rather than about the ones that happened to get through.
#
# **It writes a few keys many times over, and not many keys once each.** That is the whole of what
# makes divergence reachable. A key written once has one value, so no fault can leave two copies of
# it holding two different things: the store would have to invent bytes. A key written a hundred
# times has a hundred values, and a copy that was out of the membership for one of them holds
# something **different** from the others rather than holding nothing — which is the state that has
# to be repaired, and the half of `import_records` that repairs it is the one that decides between
# the record a reconcile file carries and the record already under that key. A suite of keys
# written once never reaches that code at all: a lagging copy is simply missing them, and the file
# is taken whole.
#
# So it writes two populations, because they are two mechanisms and neither covers the other:
#
#   hot-<stamp>-<k>     CHAOS_STORM_KEYS keys, a new value every time round
#   storm-<stamp>-<i>   a fresh key every CHAOS_STORM_SPREAD writes, written once and never again
#
# And it asserts, in the order of what losing it would mean:
#
#   no read failed                                             a copy always answered
#   every write the client issued is there, holding its value  the retry got there in the end
#   no copy of a key disagrees with another about it           nothing diverged
#   every hot key holds the value last acknowledged for it     no copy stayed behind
#   every zone holds every one of those keys, one node apiece  no copy is missing one
#
# The third and fourth are the ones the faults are chosen for, and the fourth is the sharper: a
# copy holding an **older** value is what a fault can actually make, and what has to take it away
# is `record::is_newer`. Both are given CHAOS_CONVERGE, because a copy that was away is entitled to
# lag until the reconcile pass its rejoining starts has caught it up — **lagging is allowed and
# staying behind is not**, and the budget is what tells them apart. A copy still behind when it
# runs out is divergence nothing would ever repair: no read repair, no anti-entropy, and a
# reconcile pass moves the records whose owner moved.
#
# **Reads are asserted on here, and nowhere else in this suite.** The rule is that a read that was
# not answered 2xx fails the experiment, and the one thing that excuses one is the window the load
# balancer itself owns: a target whose container has just been killed goes on being chosen until
# its health check has failed twice, which is HealthCheckIntervalSeconds × UnhealthyThresholdCount
# in cloudformation.yaml — ten seconds and two checks, so twenty, and CHAOS_STORM_GRACE is that
# with five seconds for a request already in flight. A cut off node is given the membership lease
# on top, and only that: it answers /health as normal until it has decided it is unled, and it
# cannot decide that sooner than a lease. Every failure inside one of those windows is counted and
# printed by status and by who refused; every failure outside one of them is a failed assertion.
#
# **Every fault here leaves at least one whole zone answering**, which is what makes that rule a
# claim about the database rather than about arithmetic: a key's copies are one node per zone, so
# a zone with both its nodes serving holds a copy of every key. Nothing in this experiment kills
# two zones at once, and nothing in it terminates an instance whose copy is not held elsewhere.
#
#   CHAOS_STORM_KEYS    how few keys are written over and over                 8
#   CHAOS_STORM_SPREAD  a key written once every this many of those            10
#   CHAOS_STORM_PAUSE   seconds between the client's writes                     0.5
#   CHAOS_STORM_RETRY   seconds between the retries of a refused write          2
#   CHAOS_STORM_GRACE   the window the load balancer's own health check owns    25 seconds
#   CHAOS_STORM_ROLLS   times round the zones killing both containers of one    2
#   CHAOS_STORM_SETTLE  seconds between one kill and the next                   20

source "$(dirname "$0")/harness.sh"

banner "Faults overlap and the client never gives up" \
	"Every write is retried until it is taken, and every copy has to end up holding it."

# Every read is kept with the moment it was made, because what excuses a failed one is when it
# happened. EPOCHSECONDS is what makes that free — a `date` a read is a process a read.
[ -n "${EPOCHSECONDS:-}" ] || die "This experiment needs bash 5 for EPOCHSECONDS."

setup

# **This experiment is its client**, so there is no reading it against an idle cluster: every
# assertion it makes is about what that client was told. CHAOS_LOAD=0 skips it rather than running
# it with nothing to assert on — and never in a validating run, which applies nothing and starts no
# client either.
if [ "${CHAOS_LOAD:-1}" = 0 ] && [ "${CHAOS_VALIDATE:-0}" != 1 ]; then
	echo "CHAOS_LOAD is 0, and the client that retries is the experiment. Skipping."
	exit 77
fi

seed

pause=${CHAOS_STORM_PAUSE:-0.5}
retry=${CHAOS_STORM_RETRY:-2}

# **How few keys the client writes over and over, which is the whole of what makes divergence
# reachable.** A key written once has one value, so no fault can leave two copies of it holding two
# different things — the store would have to invent bytes. A key written a hundred times has a
# hundred, and every one of them after the first arrives at a copy that already holds something
# under that key, which is the half of `import_records` that **decides between two records** rather
# than the half that takes a file whole. Eight is enough to land in several partitions, and so
# under several leaders and several owners, while staying small enough to read every copy of back.
hot_keys=${CHAOS_STORM_KEYS:-8}

# A fresh key every so many, which is the other population and a different mechanism: a copy that
# is **missing** a key catches up by a reconcile fetch taking a file whole, where a copy that is
# **stale** catches up by the version comparison. Neither covers the other.
spread=${CHAOS_STORM_SPREAD:-10}

# ALBTargetGroup in cloudformation.yaml: HealthCheckIntervalSeconds 10, UnhealthyThresholdCount 2.
# Twenty seconds is the longest a target can be chosen after it stopped answering, and the five on
# top of it is a request that was already in flight when the last check failed.
grace=${CHAOS_STORM_GRACE:-25}

rolls=${CHAOS_STORM_ROLLS:-2}
between=${CHAOS_STORM_SETTLE:-20}

# ---------------------------------------------------------------------------- the client

# The load the harness keeps is not this one, so this experiment starts its own and hands the two
# process ids to the harness variables: stop_load is what ends them, from the exit trap as well as
# from here. The difference is the whole experiment — **a write is retried until it is taken** —
# and the reads are kept with the moment they were made, because what excuses a failed read is when
# it happened and nothing else.
storm_stamp=
storm_reads=0
storm_answered=0
storm_mark=0
storm_refused_mark=0

storm_reader()
{
	local key answered code body class

	while :; do
		key=$((RANDOM % records))

		answered=$(curl --silent --max-time 15 --write-out '\n%{http_code}' \
			"$base/table/$table/key/$key")
		code=${answered##*$'\n'}

		printf '%s %s\n' "$EPOCHSECONDS" "$code" >> "$work/reads"

		case $code in
			2*) continue ;;
		esac

		# Who refused, which the status alone does not say. A 404 is the database: the API
		# answers one with no body at all, and every key read here exists. Everything else is
		# told apart by the document — nginx answers 500, 502, 503 and 504 out of
		# server/50x.json, whose code is `unavailable` and means the database on that node was
		# not there, and what carries no document at all is the load balancer answering for a
		# target that is not either.
		body=${answered%$'\n'*}

		if [ "$code" = 404 ]; then
			class=database
		else
			case $(printf '%s' "$body" | jq -r '.error.code // empty' 2> /dev/null) in
				'') class=balancer ;;
				unavailable) class=proxy ;;
				*) class=database ;;
			esac
		fi

		printf '%s %s %s %s\n' "$EPOCHSECONDS" "$code" "$class" "$key" >> "$work/reads.failed"
	done
}

# storm_put <key> <value> [deadline] — a write, retried until the cluster takes it. **The retry
# writes the same bytes again**, never a new value: a write that was refused may still have been
# taken by one copy, and what puts the other copies right is that exact record arriving again. A
# retry carrying something else would be a second value for the key rather than a repair of the
# first. Without a deadline it never gives up, which is the client this experiment is; stop_storm
# passes one, because a run cannot hang on a cluster that is never coming back.
storm_attempts=0
storm_seconds=0

storm_put()
{
	local key=$1 value=$2 deadline=${3:-} code started=$EPOCHSECONDS

	storm_attempts=0

	while :; do
		storm_attempts=$((storm_attempts + 1))

		code=$(status --request PUT --data "$value" \
			--header 'Content-Type: application/octet-stream' \
			"$base/table/$load_table/key/$key")

		case $code in
			2*) storm_seconds=$((EPOCHSECONDS - started)); return 0 ;;
		esac

		printf '%s %s %s\n' "$EPOCHSECONDS" "$code" "$key" >> "$work/refused"

		[ -z "$deadline" ] || [ "$SECONDS" -lt "$deadline" ] || return 1

		sleep "$retry"
	done
}

# **The writer blocks on the key it is holding.** It does not move on and come back, because a key
# it had moved on from is one the experiment would have to decide what to make of: what is asserted
# below is that every write it issued was taken, and a queue of its own would be a second answer to
# that in the client rather than in the cluster. The cost is that a cluster refusing every write is
# a client hammering one key, which is the load that fault has.
#
# Two populations, and they are two different mechanisms:
#
#   hot-<stamp>-<k>     a few keys, a new value every time round        the version comparison
#   storm-<stamp>-<i>   a fresh key every `spread` writes, written once  a file taken whole
#
# A hot key is what can diverge. Its copies hold a succession of values, so a fault that leaves one
# copy behind leaves it holding something **different** from the others rather than holding
# nothing — and what has to put that right is `record::is_newer` deciding between the record a
# reconcile file carries and the record the store already has under that key. Nothing else here
# reaches that: a key written once is a key every lagging copy is simply missing.
storm_writer()
{
	local n=0 i=0 k

	while :; do
		n=$((n + 1))
		k=$((n % hot_keys))

		printf '%s %s\n' "$k" "$n" >> "$work/hot.issued"

		storm_put "hot-$storm_stamp-$k" "$storm_stamp-$k-$n"

		printf '%s %s\n' "$k" "$n" >> "$work/hot"

		if [ "$((n % spread))" = 0 ]; then
			i=$((i + 1))

			echo "$i" >> "$work/issued"

			storm_put "storm-$storm_stamp-$i" "$storm_stamp-$i"

			printf '%s %s %s\n' "$i" "$storm_attempts" "$storm_seconds" >> "$work/taken"
		fi

		sleep "$pause"
	done
}

start_storm()
{
	local dropped created

	# Dropped and made again, as start_load does it: the table holds this experiment's writes and
	# nothing else, so the walk that reads every copy of it back grows with the experiment rather
	# than with everything that ran before it.
	dropped=$(status --request DELETE "$base/table/$load_table")

	case $dropped in
		204 | 404) ;;
		*) die "Could not drop $load_table: $dropped." ;;
	esac

	created=$(status --request PUT --header 'Content-Type: application/json' \
		--data '{}' "$base/table/$load_table")

	case $created in
		200 | 201) ;;
		*) die "Could not create $load_table: $created." ;;
	esac

	storm_stamp=$RANDOM

	: > "$work/reads"
	: > "$work/reads.failed"
	: > "$work/issued"
	: > "$work/taken"
	: > "$work/hot.issued"
	: > "$work/hot"
	: > "$work/refused"
	: > "$work/windows"

	storm_reader &
	reader=$!

	storm_writer &
	writer=$!

	echo "A client is reading $table and writing $load_table — $hot_keys keys over and over and a"
	echo "fresh one every $spread — and it retries a refused write until the cluster takes it."
}

# stop_storm <timeout> — the loops are stopped, and then whatever the writer was stopped holding
# is finished. Every write it ever issued is one the cluster acknowledged afterwards, which is what
# every assertion below is entitled to ask about all of them for — and, for a hot key, what makes
# the **last** value it was told had been taken a thing every copy has to end up holding.
stop_storm()
{
	local deadline=$((SECONDS + $1)) key k n outstanding

	stop_load

	sort -u "$work/issued" > "$work/issued.keys"
	awk '{ print $1 }' "$work/taken" | sort -u > "$work/taken.keys"
	comm -23 "$work/issued.keys" "$work/taken.keys" > "$work/outstanding"

	sort -u "$work/hot.issued" > "$work/hot.issued.keys"
	sort -u "$work/hot" > "$work/hot.taken.keys"
	comm -23 "$work/hot.issued.keys" "$work/hot.taken.keys" > "$work/hot.outstanding"

	outstanding=$(( $(grep -c . "$work/outstanding") + $(grep -c . "$work/hot.outstanding") ))

	if [ "$outstanding" != 0 ]; then
		echo "  Finishing the $outstanding write the client was stopped holding."

		while read -r key; do
			storm_put "storm-$storm_stamp-$key" "$storm_stamp-$key" "$deadline" \
				&& printf '%s %s %s\n' "$key" "$storm_attempts" "$storm_seconds" >> "$work/taken"
		done < "$work/outstanding"

		while read -r k n; do
			storm_put "hot-$storm_stamp-$k" "$storm_stamp-$k-$n" "$deadline" \
				&& printf '%s %s\n' "$k" "$n" >> "$work/hot"
		done < "$work/hot.outstanding"

		awk '{ print $1 }' "$work/taken" | sort -u > "$work/taken.keys"
		comm -23 "$work/issued.keys" "$work/taken.keys" > "$work/outstanding"

		sort -u "$work/hot" > "$work/hot.taken.keys"
		comm -23 "$work/hot.issued.keys" "$work/hot.taken.keys" > "$work/hot.outstanding"
	fi

	expect "$(( $(grep -c . "$work/outstanding") + $(grep -c . "$work/hot.outstanding") ))" 0 \
		"every write the client issued was taken in the end"

	# The last value the cluster acknowledged for each hot key, and what a copy of it has to hold.
	# Every line in $work/hot was acknowledged, so the highest is the last.
	awk '{ last[$1] = $2 } END { for (k in last) print k, last[k] }' "$work/hot" \
		| sort -n > "$work/hot.last"

	awk -v stamp="$storm_stamp" '{ printf "hot-%s-%s %s-%s-%s\n", stamp, $1, stamp, $1, $2 }' \
		"$work/hot.last" | sort > "$work/hot.expected"

	printf '  ---- %s keys were written %s times between them, and %s more were written once\n' \
		"$(grep -c . "$work/hot.last")" "$(grep -c . "$work/hot")" "$(grep -c . "$work/taken.keys")"
}

# storm_report <description> — what the client saw since the last report, which is what makes these
# lines a phase rather than a running total.
storm_report()
{
	local reads answered lines writes refused retried

	reads=$(grep -c . "$work/reads") || reads=0
	answered=$(grep -c ' 2[0-9][0-9]$' "$work/reads") || answered=0
	: > "$work/reads"

	storm_reads=$((storm_reads + reads))
	storm_answered=$((storm_answered + answered))

	lines=$(grep -c . "$work/taken") || lines=0
	writes=$((lines - storm_mark))
	storm_mark=$lines

	refused=$(grep -c . "$work/refused") || refused=0
	retried=$((refused - storm_refused_mark))
	storm_refused_mark=$refused

	printf '  ---- %s: %s of %s reads were answered 2xx, %s writes were taken, %s were refused and retried\n' \
		"$1" "$answered" "$reads" "$writes" "$retried"
}

# ---------------------------------------------------------------------------- the read assertion

# storm_excuse <from> <seconds> <what> — the window in which a read that was not answered 2xx is
# the load balancer still choosing a target it has not yet been told to stop choosing. It opens
# when the fault was applied and closes <seconds> after the call that applied it came back, which
# for a kill is after the node is answering again: the whole of that is a node that is down.
storm_excuse()
{
	printf '%s %s %s\n' "$1" "$((EPOCHSECONDS + $2))" "$3" >> "$work/windows"
}

# **No read failed.** Every failure is kept with the moment it happened, and a failure inside one
# of the windows above is counted and printed rather than asserted on — outside them there is
# nothing left to blame but the database, which is why this is the one assertion about reads in
# the suite.
expect_reads_held()
{
	local failed outside excused

	failed=$(grep -c . "$work/reads.failed") || failed=0

	: > "$work/reads.inside"

	# A failure falls in a window or it does not. awk rather than a loop because a fault costs
	# hundreds of them and a process apiece is minutes.
	#
	# The empty case is written out rather than left to awk: `NR == FNR` reads the second file as
	# the first when the first is empty, which would excuse a failure by matching it against
	# itself. No window at all is every failure outside one, which is the answer a run that
	# applied no fault has to give.
	if [ -s "$work/windows" ]; then
		awk -v inside="$work/reads.inside" \
			'NR == FNR { from[NR] = $1; to[NR] = $2; windows = NR; next }
			{
				for (i = 1; i <= windows; i++)
				{
					if ($1 >= from[i] && $1 <= to[i]) { print > inside; next }
				}

				print
			}' "$work/windows" "$work/reads.failed" > "$work/reads.outside"
	else
		cp "$work/reads.failed" "$work/reads.outside"
	fi

	outside=$(grep -c . "$work/reads.outside") || outside=0
	excused=$((failed - outside))

	printf '  ---- %s of %s reads were answered 2xx over the whole experiment\n' \
		"$storm_answered" "$storm_reads"

	if [ "$excused" != 0 ]; then
		printf '  ---- %s failed reads fell inside the %s windows the load balancer owns:\n' \
			"$excused" "$(grep -c . "$work/windows")"

		awk '{ print $2, $3 }' "$work/reads.inside" | sort | uniq -c | sort -rn \
			| awk '{ printf "       %s×%s, refused by the %s\n", $1, $2, $3 }'
	fi

	if [ "$outside" = 0 ]; then
		result 0 "no read failed outside the window the load balancer takes to notice a target"
	else
		result 1 "no read failed outside the window the load balancer takes to notice a target — $outside did"

		awk '{ print $2, $3 }' "$work/reads.outside" | sort | uniq -c | sort -rn | head -5 \
			| awk '{ printf "       %s×%s, refused by the %s\n", $1, $2, $3 }'
	fi
}

# ---------------------------------------------------------------------------- the write assertions

# expect_storm_kept <when> — every key the client issued reads back the value it wrote. There is no
# key here the cluster is entitled to have lost: every one of them was acknowledged, which is a
# write every copy took, and nothing in this experiment terminates an instance.
#
# The readback is one curl over one connection rather than a request a process, as load_survivors
# does it: the client issues thousands of keys over an experiment this long.
expect_storm_kept()
{
	local taken lost=0 wrong=0 key code attempt

	taken=$(grep -c . "$work/issued.keys") || taken=0

	expect_not "$taken" 0 "the client got writes taken while the faults overlapped"

	[ "$taken" = 0 ] && return 0

	rm -rf "$work/back"
	mkdir -p "$work/back"

	while read -r key; do
		printf 'url = "%s"\noutput = "%s"\n' \
			"$base/table/$load_table/key/storm-$storm_stamp-$key" "$work/back/$key"
	done < "$work/issued.keys" > "$work/readback"

	curl --silent --max-time 120 --config "$work/readback" --write-out '%{http_code}\n' \
		> "$work/back.codes"

	paste "$work/issued.keys" "$work/back.codes" > "$work/back.status"

	awk '$2 !~ /^2[0-9][0-9]$/ { print $1 }' "$work/back.status" | sort > "$work/missed"

	# The value is the key's own number, so what a record should hold is worked out from the name
	# of the file it was read into.
	awk -v stamp="$storm_stamp" '
		FNR == 1 { key = FILENAME; sub(/.*\//, "", key); if ($0 != stamp "-" key) print key }' \
		"$work/back"/* | sort > "$work/unexpected"

	# A key that did not answer 2xx holds an error document rather than a value, so it is asked
	# again below and is not a value that changed.
	wrong=$(comm -23 "$work/unexpected" "$work/missed" | grep -c .) || wrong=0

	while read -r key; do
		for attempt in 1 2 3; do
			code=$(read_value "$load_table" "storm-$storm_stamp-$key")

			case $code in
				2*) break ;;
			esac
		done

		case $code in
			2*) [ "$(cat "$work/value")" = "$storm_stamp-$key" ] || wrong=$((wrong + 1)) ;;
			*) lost=$((lost + 1)) ;;
		esac
	done < "$work/missed"

	expect "$lost" 0 "every write the client got taken $1 is still there"
	expect "$wrong" 0 "and every one of them reads back what was written"

	printf '  ---- %s write-once keys, %s refused at least once, the slowest took %ss to be taken\n' \
		"$taken" \
		"$(awk '$2 > 1' "$work/taken" | grep -c .)" \
		"$(awk 'BEGIN { worst = 0 } $3 > worst { worst = $3 } END { print worst + 0 }' "$work/taken")"
}

# expect_hot_settled <when> — **the divergence assertion, and the reason the client hammers a few
# keys.** Every node is asked what it holds of the load table in its own store, and two things have
# to be true of it: no key is held at two different values, and every hot key holds the last value
# the cluster acknowledged for it.
#
# It is given CHAOS_CONVERGE, and **that is the difference between this and `copies_agree`**. There,
# every key is written once and never again, so two copies holding two values is a cluster that
# invented one and no amount of waiting would put it right — the question can be asked the moment
# the load stops. Here a copy that was out of the membership for a write legitimately holds the
# value before it, and what brings it up to date is the reconcile fetch its rejoining starts:
# `import_records` walking the file against what the store already holds and keeping whichever
# `record::is_newer` says. So lagging is allowed and **staying behind is not**, and the budget is
# what tells them apart. A copy still holding an older value when it runs out is divergence that
# nothing in the cluster would ever repair: there is no read repair, no anti-entropy, and a
# reconcile pass moves the records whose owner moved.
expect_hot_settled()
{
	local deadline=$((SECONDS + converge)) id key expected disagreed count wrong values

	while :; do
		rm -rf "$work/values"
		mkdir -p "$work/values"

		holdings_asked=0
		holdings_total=0

		while read -r id _; do
			holdings_total=$((holdings_total + 1))

			# The value is carried base64, because what a client wrote is bytes of its choosing
			# and what is compared here is a line.
			if walk_store "$id" "$load_table" true '.key + " " + (.value | @base64)' \
				> "$work/values/$id"; then
				holdings_asked=$((holdings_asked + 1))
			fi
		done < <(instances asyncdb)

		# Every copy every node answered for, and then one line per key and value held anywhere:
		# the copies of a key that agree collapse onto one line, so a key still named twice is one
		# two nodes answered differently for.
		cat "$work/values"/* 2> /dev/null > "$work/copies"
		sort -u "$work/copies" > "$work/held"

		disagreed=$(awk '{ print $1 }' "$work/held" | uniq -d)
		count=$(echo "$disagreed" | grep -c .) || count=0

		: > "$work/hot.wrong"

		while read -r key expected; do
			mapfile -t values < <(awk -v k="$key" '$1 == k { print $2 }' "$work/held")

			if [ "${#values[@]}" = 0 ]; then
				printf '%s is held by no node at all\n' "$key" >> "$work/hot.wrong"
			elif [ "${#values[@]}" != 1 ]; then
				printf '%s is held at %s values:%s\n' "$key" "${#values[@]}" \
					"$(for one in "${values[@]}"; do printf ' %s' "$(echo "$one" | base64 -d)"; done)" \
					>> "$work/hot.wrong"
			elif [ "$(echo "${values[0]}" | base64 -d)" != "$expected" ]; then
				printf '%s holds %s, and %s was the last acknowledged\n' "$key" \
					"$(echo "${values[0]}" | base64 -d)" "$expected" >> "$work/hot.wrong"
			fi
		done < "$work/hot.expected"

		wrong=$(grep -c . "$work/hot.wrong") || wrong=0

		[ "$count" != 0 ] || [ "$wrong" != 0 ] || break
		[ "$holdings_asked" = "$holdings_total" ] || break
		[ "$SECONDS" -lt "$deadline" ] || break

		sleep 15
	done

	expect "$holdings_asked" "$holdings_total" "every node said what it holds of $load_table $1"

	# Printed whether it passed or failed, because an agreement over nothing is the answer a walk
	# that found an empty store on every node would also give.
	printf '  ---- %s copies of %s keys answered out of their own stores %s\n' \
		"$(grep -c . "$work/copies")" "$(awk '{ print $1 }' "$work/held" | sort -u | grep -c .)" "$1"

	if [ "$count" = 0 ]; then
		result 0 "no copy of a key disagrees with another about what is in it $1"
	else
		result 1 "no copy of a key disagrees with another about what is in it $1 — held at two values: $count"

		echo "$disagreed" | head -3 | while read -r key; do
			printf '       %s is held as %s\n' "$key" "$(awk -v k="$key" '$1 == k { print $2 }' \
				"$work/held" | while read -r one; do printf '%s ' "$(echo "$one" | base64 -d)"; done)"
		done
	fi

	if [ "$wrong" = 0 ]; then
		result 0 "every key written over and over holds the value last acknowledged for it $1"
	else
		result 1 "every key written over and over holds the value last acknowledged for it $1 — $wrong do not"

		head -5 "$work/hot.wrong" | sed 's/^/       /'
	fi
}

# expect_hot_read <when> — the same claim as a client sees it, through the load balancer, which
# answers from whichever copy has the key. It is weaker than the walk above by construction and it
# is the one a client would notice: a read that comes back with a value two writes old is the
# divergence arriving at somebody's request rather than in a store.
expect_hot_read()
{
	local key expected code wrong=0

	while read -r key expected; do
		code=$(read_value "$load_table" "$key")

		case $code in
			2*) [ "$(cat "$work/value")" = "$expected" ] || wrong=$((wrong + 1)) ;;
			*) wrong=$((wrong + 1)) ;;
		esac
	done < "$work/hot.expected"

	expect "$wrong" 0 "every one of them reads back that value through the load balancer $1"
}

# expect_storm_replicated <when> — every zone holds every key the client issued, and no key is in
# two stores of one zone. It is the half copies_agree cannot see: a copy that is **missing** a key
# is not a disagreement, and a zone that was cut off missed every write taken while it was gone.
#
# What fetches them is the reconcile pass that a rejoining node's membership change starts, and it
# fetches every partition this node owns from a holder in each zone rather than only the ones whose
# owner moved — so it is given CHAOS_CONVERGE the way expect_copies is.
expect_storm_replicated()
{
	local deadline=$((SECONDS + converge)) zone missing short duplicates

	{
		sed "s/^/storm-$storm_stamp-/" "$work/issued.keys"
		awk -v stamp="$storm_stamp" '{ print "hot-" stamp "-" $1 }' "$work/hot.last"
	} | sort -u > "$work/wanted"

	while :; do
		short=

		collect_holdings "$load_table"
		duplicates=$(zone_duplicates)

		for zone in $(zones_held); do
			cat "$work/holdings/$zone."* 2> /dev/null | sort -u > "$work/held.$zone"

			missing=$(comm -23 "$work/wanted" "$work/held.$zone" | grep -c .)

			[ "$missing" = 0 ] || short="$short $zone($missing)"
		done

		[ -n "$short" ] || [ -n "$duplicates" ] || break
		[ "$holdings_asked" = "$holdings_total" ] || break
		[ "$SECONDS" -lt "$deadline" ] || break

		sleep 15
	done

	expect "$holdings_asked" "$holdings_total" "every node said what it holds of $load_table $1"

	if [ -z "$short" ]; then
		result 0 "every write the client issued is in every zone $1"
	else
		result 1 "every write the client issued is in every zone $1 — short:$short"
	fi

	if [ -z "$duplicates" ]; then
		result 0 "no key is held by two nodes of a zone $1"
	else
		result 1 "no key is held by two nodes of a zone $1 — held twice: $duplicates"
	fi
}

# ---------------------------------------------------------------------------- the targets

zone_nodes()
{
	instances asyncdb | awk -v z="$1" '$2 == z { print $1 }'
}

mapfile -t zones < <(instances asyncdb | awk '{ print $2 }' | sort -u)

all_ids=$(instances asyncdb | cut -f1)
count=$(echo "$all_ids" | grep -c .)

expect "$count" 6 "the stack is running six database nodes"
expect "${#zones[@]}" 3 "in three availability zones"
{ [ "$count" = 6 ] && [ "${#zones[@]}" = 3 ]; } || { verdict; exit 1; }

# Which zone plays which part. **No fault here touches two of them at once**: the zone that is cut
# off keeps its containers, the zone whose containers are killed keeps its network, and the third
# is whole throughout the first two rounds — which is the zone that answers every read while they
# stand, because a zone with both its nodes serving holds a copy of every key.
cut_zone=${zones[0]}
kill_zone=${zones[1]}
third_zone=${zones[2]}

cut_subnet=$(instances asyncdb | awk -v z="$cut_zone" '$2 == z { print $3; exit }')
victim=$(zone_nodes "$kill_zone" | head -1)

echo "  $cut_zone is the zone that is cut off, $kill_zone is where the containers are killed,"
echo "  and $victim is the instance that is stopped."

# storm_kill <what> <instance>... — one kill, the window it opens, and what each node said. **A
# kill that could not be sent is a failed assertion and not the end of the storm**: an experiment
# this long that abandoned twenty minutes of faults over one Run Command would report nothing about
# any of them, and every round after this one is still worth applying. The window opens either way,
# because a kill that half landed still took nodes down.
storm_kill()
{
	local what=$1 from=$EPOCHSECONDS node
	shift

	if ! kill_containers "$@"; then
		result 1 "the kill of $what reached every node it was sent to"
	fi

	storm_excuse "$from" "$grace" "$what"

	for node in "$@"; do
		printf '  ---- %s: %s\n' "$node" \
			"$(tr '\n' ' ' < "$work/answer.$node" 2> /dev/null || echo 'said nothing')"
	done
}

# ---------------------------------------------------------------------------- the fault

# The storm, and it is the whole of the fault: three rounds, each of them two faults standing at
# once, applied while the client is reading and writing. Nothing here waits a fault out — each one
# is taken away as soon as the round it belongs to is done, which is what lets the next round land
# on a cluster that is short of something else.
inject()
{
	local from node roll zone pair

	# ---- One. A zone cut off, and the containers of another zone killed under it, one node at a
	# time so that the zone never loses both copies it holds at once. The cut zone falls out of
	# the membership on its lease, so the writes the client is retrying are taken by two zones —
	# and the cut zone is holding a store that is missing every one of them by the time it is let
	# back in.
	echo "  Round one: cutting off $cut_zone, then killing the containers in $kill_zone under it."

	from=$EPOCHSECONDS

	zone_cut "$cut_subnet" "$cut_zone" || return 1

	# The isolated side answers /health as normal until it has decided it is unled, which it
	# cannot do sooner than a membership lease, and only then can its health check start failing.
	storm_excuse "$from" "$((grace + lease))" "$cut_zone was cut off"

	sleep "$onset"

	for node in $(zone_nodes "$kill_zone"); do
		storm_kill "a container in $kill_zone" "$node"

		sleep "$between"
	done

	storm_report "while $cut_zone was cut off and $kill_zone was being killed under it"

	zone_heal

	# What the cut zone has to do now is catch up on every write it missed, and the pass that does
	# it starts from the membership change its rejoining is. The round after this one lands on
	# top of that, deliberately.
	await '(.zones | length) == 3 and (.nodes | length) == 6' "$settle" \
		"$cut_zone rejoined once it was let back in"

	# ---- Two. An instance stopped — which the group answers by terminating it and launching a
	# replacement, an empty store and a rebuild — and a container killed in the third zone while
	# that is happening. The zone that was cut off in round one is whole here and answers the
	# reads.
	echo "  Round two: stopping $victim, and killing a container in $third_zone while it goes."

	from=$EPOCHSECONDS

	aws ec2 stop-instances --instance-ids "$victim" > /dev/null || return 1

	storm_excuse "$from" "$grace" "$victim was stopped"

	sleep "$onset"

	storm_kill "a container in $third_zone" "$(zone_nodes "$third_zone" | head -1)"

	storm_report "while $victim was being replaced and $third_zone was being killed"

	echo "  Waiting for the group to replace $victim."

	# Reported rather than returned: a replacement that never arrives is a failed assertion and
	# not a fault that could not be applied, and the rounds after it are still worth running
	# against the five nodes that are left.
	await '(.nodes | length) == 6 and (.zones | length) == 3' "$recovery" \
		"the group replaced the stopped instance and it rejoined"

	# ---- Three. Every container of one zone killed at once, a zone at a time, round and round.
	# A zone that loses both its nodes loses the copy it holds, so **every write in the cluster is
	# refused for as long as it is down** — a write needs every copy — and what the client does
	# about that is the whole point of it.
	for (( roll = 1; roll <= rolls; roll++ )); do
		for zone in "${zones[@]}"; do
			mapfile -t pair < <(zone_nodes "$zone")

			[ "${#pair[@]}" -gt 0 ] || continue

			echo "  Round three, pass $roll of $rolls: killing every container in $zone."

			storm_kill "every container in $zone" "${pair[@]}"

			sleep "$between"
		done

		storm_report "while every zone was killed in turn, pass $roll"
	done
}

# Every round takes its own fault away as it ends, so this is the run that died holding one. The
# acl is the only one of the three that stands by itself; a stopped instance the group has not
# replaced is started, and a container the restart policy did not bring back is started too.
heal()
{
	local stopped ids

	zone_heal

	stopped=$(aws ec2 describe-instances --instance-ids "$victim" \
		--query 'Reservations[].Instances[].State.Name' --output text 2> /dev/null)

	[ "$stopped" = stopped ] && aws ec2 start-instances --instance-ids "$victim" > /dev/null 2>&1

	ids=$(instances asyncdb | cut -f1)

	# shellcheck disable=SC2086
	[ -n "$ids" ] && container_start $ids

	return 0
}

preflight()
{
	local answered

	may_write_acls
	may_stop "$victim"

	# shellcheck disable=SC2086
	may_run $all_ids

	answered=$(ssm_run "$(echo "$all_ids" | head -1)" \
		"docker ps --format '{{.Image}}' | grep -c asyncdb")

	expect "${answered:-0}" 1 "one container of the asyncdb image is running on the first node"
}

# The client is started after the preflight, because a validating run applies nothing and has
# nothing for a client to be doing.
[ "${CHAOS_VALIDATE:-0}" = 1 ] || start_storm

fault_start || { verdict; exit 1; }

fault_stop

# ---------------------------------------------------------------------------- what is left

await '(.nodes | length) == 6 and (.zones | length) == 3' "$recovery" \
	"the cluster is six nodes in three zones again once the storm is over"

await_writes 20 "$settle" "every copy takes a write again"

stop_storm "$settle"

storm_report "once the storm was over"

expect_reads_held

expect_storm_kept "while the faults overlapped"

# What the readback above cannot see. It is answered by whichever copy has the key, so it says a
# write survived somewhere; this asks each node out of its own store. The seeded table is not
# asked: every write to it is the same value, so two copies of one of its keys cannot differ
# whatever happened to them, which is exactly the reason the client here writes as it does.
expect_hot_settled "once the storm was over"

expect_hot_read "once the storm was over"

expect_storm_replicated "once the storm was over"

expect_round_trip 10 "a key written after the storm reads back what was written"

verdict
