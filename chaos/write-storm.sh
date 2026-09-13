#! /usr/bin/env bash

# Faults that overlap, under a client that never gives up on a write.
#
#   doc/runbook/index.md#what-recovers-by-itself
#   doc/runbook/membership.md#etcd-cannot-be-reached
#   doc/runbook/membership.md#the-membership-is-wrong
#   doc/runbook/nodes.md#an-instance-was-replaced
#   doc/runbook/rebuild.md#when-ownership-moves

source "$(dirname "$0")/harness.sh"

banner "Faults overlap and the client never gives up" \
	"Every write is retried until it is taken, and every copy has to end up holding it."

# EPOCHSECONDS keeps the moment of every read without a `date` a read, which is a process a read.
[ -n "${EPOCHSECONDS:-}" ] || die "This experiment needs bash 5 for EPOCHSECONDS."

setup

# A validating run starts no client, so CHAOS_LOAD says nothing to it.
if [ "${CHAOS_LOAD:-1}" = 0 ] && [ "${CHAOS_VALIDATE:-0}" != 1 ]; then
	echo "CHAOS_LOAD is 0, and the client that retries is the experiment. Skipping."
	exit 77
fi

seed

pause=${CHAOS_STORM_PAUSE:-0.5}
retry=${CHAOS_STORM_RETRY:-2}

# Eight is enough to land in several partitions, and so under several leaders and several owners,
# while staying small enough to read every copy of back.
hot_keys=${CHAOS_STORM_KEYS:-8}

spread=${CHAOS_STORM_SPREAD:-10}
grace=${CHAOS_STORM_GRACE:-10}
rolls=${CHAOS_STORM_ROLLS:-2}
between=${CHAOS_STORM_SETTLE:-20}
hold=${CHAOS_STORM_HOLD:-60}

# ---------------------------------------------------------------------------- the client

# The load the harness keeps is not this one, so this experiment starts its own and hands the two
# process ids to the harness variables: stop_load is what ends them, from the exit trap as well as
# from here.
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

		# Longer than the load balancer's idle timeout, so a read it left waiting on a target that
		# went away is its 504 and not a client that gave up first.
		answered=$(curl --silent --max-time 20 --write-out '\n%{http_code}' \
			"$base/table/$table/key/$key")
		code=${answered##*$'\n'}

		printf '%s %s\n' "$EPOCHSECONDS" "$code" >> "$work/reads"

		case $code in
			2*) continue ;;
		esac

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

# storm_put <key> <value> [deadline] — a write, retried with the same bytes until the cluster takes
# it. Without a deadline it never gives up, which is the client this experiment is; stop_storm
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
# is finished.
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

	# Both populations, because the refusals below count both: a phase line saying six writes were
	# taken and eleven refused is one counting a tenth of what was written against all of what was
	# refused, which reads as a cluster refusing more than it took.
	lines=$(( $(grep -c . "$work/hot") + $(grep -c . "$work/taken") ))
	writes=$((lines - storm_mark))
	storm_mark=$lines

	refused=$(grep -c . "$work/refused") || refused=0
	retried=$((refused - storm_refused_mark))
	storm_refused_mark=$refused

	printf '  ---- %s: %s of %s reads were answered 2xx, %s writes were taken, %s were refused and retried\n' \
		"$1" "$answered" "$reads" "$writes" "$retried"
}

# ---------------------------------------------------------------------------- the read assertion

# storm_excuse <from> <seconds> <what> — a window in which a read that was not answered 2xx is
# excused.
storm_excuse()
{
	printf '%s %s %s\n' "$1" "$((EPOCHSECONDS + $2))" "$3" >> "$work/windows"
}

expect_reads_held()
{
	local refused failed outside excused

	awk '$3 == "database"' "$work/reads.failed" > "$work/reads.database"
	awk '$3 != "database"' "$work/reads.failed" > "$work/reads.passed"

	refused=$(grep -c . "$work/reads.database") || refused=0
	failed=$(grep -c . "$work/reads.passed") || failed=0

	: > "$work/reads.inside"

	# A failure falls in a window or it does not. awk rather than a loop because a fault costs
	# hundreds of them and a process apiece is minutes.
	#
	# The empty case is written out rather than left to awk: `NR == FNR` reads the second file as
	# the first when the first is empty, which would excuse a failure by matching it against
	# itself. No window at all is every failure outside one, which is the answer a run that
	# applied no fault has to give.
	if [ -s "$work/windows" ] && [ -s "$work/reads.passed" ]; then
		awk -v inside="$work/reads.inside" \
			'NR == FNR { from[NR] = $1; to[NR] = $2; windows = NR; next }
			{
				for (i = 1; i <= windows; i++)
				{
					if ($1 >= from[i] && $1 <= to[i]) { print > inside; next }
				}

				print
			}' "$work/windows" "$work/reads.passed" > "$work/reads.outside"
	else
		cp "$work/reads.passed" "$work/reads.outside"
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

	if [ "$refused" = 0 ]; then
		result 0 "no read was refused by the database, inside a window or out of one"
	else
		result 1 "no read was refused by the database, inside a window or out of one — $refused were"

		awk '{ print $2 }' "$work/reads.database" | sort | uniq -c | sort -rn | head -5 \
			| awk '{ printf "       %s×%s\n", $1, $2 }'
	fi
}

# ---------------------------------------------------------------------------- the write assertions

# expect_storm_kept <when> — every key the client issued reads back the value it wrote.
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

# expect_hot_settled <when> — every node is asked what it holds of the load table in its own store:
# no key may be held at two different values, and every hot key has to hold the last value the
# cluster acknowledged for it.
expect_hot_settled()
{
	local deadline=$((SECONDS + converge)) id key expected disagreed count wrong values
	local started=$SECONDS behind= caught=0

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

		# What the first walk found is measured once, and is not a reason to go round again.
		if [ -z "$behind" ]; then
			behind=$((count + wrong))
		fi

		[ "$count" != 0 ] || [ "$wrong" != 0 ] || break
		[ "$holdings_asked" = "$holdings_total" ] || break
		[ "$SECONDS" -lt "$deadline" ] || break

		sleep 15
	done

	caught=$((SECONDS - started))

	expect "$holdings_asked" "$holdings_total" "every node said what it holds of $load_table $1"

	# **An assertion over nothing is the one way this passes without testing anything**, and there
	# are two of them. The first is an empty set of keys: a client that never got a write taken
	# leaves nothing to compare, and the loop above would walk every store and find no fault in
	# any of it.
	expect_not "$(grep -c . "$work/hot.expected")" 0 \
		"the client got writes taken for the keys it wrote over and over"

	printf '  ---- %s of %s keys were behind or disagreed on the first walk, settled in %ss\n' \
		"${behind:-0}" "$(grep -c . "$work/hot.expected")" "$caught"

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
# two stores of one zone.
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

		# The keys, because a count says a zone is short and never which write it lost.
		for zone in $(zones_held); do
			comm -23 "$work/wanted" "$work/held.$zone" | head -5 | sed "s/^/       $zone is missing /"
		done
	fi

	if [ -z "$duplicates" ]; then
		result 0 "no key is held by two nodes of a zone $1"
	else
		result 1 "no key is held by two nodes of a zone $1 — held twice: $duplicates"
	fi
}

# storm_lagging <a zone> <another zone> — how many hot keys those two zones hold at **different**
# values in their own stores.
#
# **It compares zones and not nodes, because a node is not a copy.** A zone holds a copy of the
# whole keyspace and its two nodes split it, and the split is hashed inside each zone — so two
# nodes picked out of two zones can hold disjoint halves and agree about nothing by holding nothing
# in common. Both nodes of each zone are walked and the answers put together, which is the copy.
storm_lagging()
{
	local one=$1 other=$2 id

	: > "$work/lag.one"
	: > "$work/lag.other"

	for id in $(zone_nodes "$one"); do
		walk_store "$id" "$load_table" true '.key + " " + (.value | @base64)' \
			> "$work/lag.node" || return 1

		grep '^hot-' "$work/lag.node" >> "$work/lag.one"
	done

	for id in $(zone_nodes "$other"); do
		walk_store "$id" "$load_table" true '.key + " " + (.value | @base64)' \
			> "$work/lag.node" || return 1

		grep '^hot-' "$work/lag.node" >> "$work/lag.other"
	done

	# join needs both sides ordered on the key, and a zone holds one value a key.
	sort -o "$work/lag.one" "$work/lag.one"
	sort -o "$work/lag.other" "$work/lag.other"

	# Distinct keys and not joined lines. A key is briefly in two stores of one zone while a
	# reconcile moves it, and the join is then a cross product — which is how eight keys were once
	# reported as thirteen differences.
	join -j 1 -o 0,1.2,2.2 "$work/lag.one" "$work/lag.other" \
		| awk '$2 != $3 { print $1 }' | sort -u | grep -c .
}

# storm_catchup <zone> — how long that zone takes to hold the last acknowledged value of every hot
# key again, once the rule keeping it out of the membership has gone.
#
# What it compares against moves, deliberately: the client is still writing, so the last value it
# was told had been taken is read fresh every sample, and read **before** the nodes are asked. A
# write is acknowledged only once every copy has taken it, so a zone that is caught up holds that
# value or a later one — and a later one is what the client will have been told about by the time
# a Run Command comes back, which is why an exact match is never the test.
storm_catchup()
{
	local zone=$1 deadline=$((SECONDS + converge)) started=$SECONDS
	local ids script behind

	mapfile -t ids < <(zone_nodes "$zone")

	[ "${#ids[@]}" != 0 ] || { printf '%s 0\n' "$hot_keys"; return 0; }

	script="for k in \$(seq 0 $((hot_keys - 1))); do
	printf '%s ' \"\$k\"
	curl -s --max-time 5 -H '$forwarded_header: true' \
		\"http://localhost:8080/table/$load_table/key/hot-$storm_stamp-\$k\"
	echo
done"

	while :; do
		awk '{ last[$1] = $2 } END { for (k in last) print k, last[k] }' "$work/hot" \
			> "$work/catchup.want"

		if ssm_all "$script" "${ids[@]}"; then
			cat "$work"/answer.* > "$work/catchup"

			# A zone holds a copy of the whole keyspace split between its nodes, so a key is
			# answered for by one of the two and the pair of them is the copy. A value is
			# <stamp>-<key>-<n>, and the node that does not hold the key answers nothing.
			behind=$(awk -v stamp="$storm_stamp" '
				NR == FNR {
					if (split($2, part, "-") == 3 && part[1] == stamp && part[2] == $1 && part[3] + 0 > held[$1] + 0)
						held[$1] = part[3]
					next
				}
				!($1 in held) || held[$1] + 0 < $2 + 0 { late++ }
				END { print late + 0 }' "$work/catchup" "$work/catchup.want")
		else
			behind=$hot_keys
		fi

		[ "$behind" = 0 ] && break
		[ "$SECONDS" -lt "$deadline" ] || break

		sleep 5
	done

	printf '%s %s\n' "$behind" "$((SECONDS - started))"
}

# ---------------------------------------------------------------------------- the targets

zone_nodes()
{
	instances asyncdb | awk -v z="$1" '$2 == z { print $1 }'
}

# Any zone but that one, which is what a zone that is out of the membership is measured against.
other_zone()
{
	instances asyncdb | awk -v z="$1" '$2 != z { print $2; exit }'
}

mapfile -t zones < <(instances asyncdb | awk '{ print $2 }' | sort -u)

all_ids=$(instances asyncdb | cut -f1)
count=$(echo "$all_ids" | grep -c .)

expect "$count" 6 "the stack is running six database nodes"
expect "${#zones[@]}" 3 "in three availability zones"
{ [ "$count" = 6 ] && [ "${#zones[@]}" = 3 ]; } || { verdict; exit 1; }

cut_zone=${zones[0]}
kill_zone=${zones[1]}
third_zone=${zones[2]}

cut_subnet=$(instances asyncdb | awk -v z="$cut_zone" '$2 == z { print $3; exit }')
victim=$(zone_nodes "$kill_zone" | head -1)

echo "  $cut_zone is the zone that is cut off, $kill_zone is where the containers are killed,"
echo "  and $victim is the instance that is stopped."

# Nothing else on a database node is forwarded to 2379, so the port alone names etcd.
deaf_match="-p tcp --dport 2379"
deaf=()

# storm_isolate <what> <instance>... — those nodes lose etcd, and are **held** that way.
storm_isolate()
{
	local what=$1 from=$EPOCHSECONDS
	shift

	deaf=("$@")

	if ! blackhole "$((hold + 300))" "$deaf_match" "$@"; then
		result 1 "$what lost etcd"
		deaf=()

		return 1
	fi

	storm_excuse "$from" "$((grace + lease))" "$what"

	printf '  ---- %s, and it is held for %ss, which is %s leases\n' "$what" "$hold" "$((hold / lease))"
}

# storm_rejoin — the rule taken away.
storm_rejoin()
{
	[ "${#deaf[@]}" != 0 ] || return 0

	blackhole_clear "$deaf_match" "${deaf[@]}"

	deaf=()
}

# ---------------------------------------------------------------------------- the fault

inject()
{
	local from node roll zone pair lagging behind caught

	# ---- One. A zone cut off, and etcd taken from another zone's nodes under it one node at a time,
	# so that zone never loses both copies it holds at once.
	echo "  Round one: cutting off $cut_zone, then taking etcd from $kill_zone's nodes under it."

	from=$EPOCHSECONDS

	zone_cut "$cut_subnet" "$cut_zone" || return 1

	storm_excuse "$from" "$((grace + lease))" "$cut_zone was cut off"

	sleep "$onset"

	for node in $(zone_nodes "$kill_zone"); do
		storm_isolate "a node in $kill_zone lost etcd" "$node" && sleep "$hold"

		storm_rejoin

		sleep "$between"
	done

	storm_report "while $cut_zone was cut off and $kill_zone was losing etcd under it"

	lagging=$(storm_lagging "$cut_zone" "$third_zone") || lagging=

	if [ -z "$lagging" ]; then
		result 1 "the two sides of the cut said what they hold"
	else
		expect_not "$lagging" 0 \
			"the cut off zone is holding older values than the zones still taking writes"

		printf '  ---- %s of the %s keys written over and over differ across the cut\n' \
			"$lagging" "$hot_keys"
	fi

	zone_heal

	# The round after this one lands on top of the cut zone catching up, deliberately.
	await '(.zones | length) == 3 and (.nodes | length) == 6' "$settle" \
		"$cut_zone rejoined once it was let back in"

	# ---- Two. An instance stopped, and etcd taken from a node of the third zone while it goes. The
	# zone that was cut off in round one is whole here and answers the reads.
	echo "  Round two: stopping $victim, and taking etcd from a node of $third_zone while it goes."

	from=$EPOCHSECONDS

	aws ec2 stop-instances --instance-ids "$victim" > /dev/null || return 1

	storm_excuse "$from" "$grace" "$victim was stopped"

	sleep "$onset"

	storm_isolate "a node in $third_zone lost etcd" "$(zone_nodes "$third_zone" | head -1)" \
		&& sleep "$hold"

	storm_rejoin

	storm_report "while $victim was being replaced and $third_zone was losing etcd"

	echo "  Waiting for the group to replace $victim."

	# Reported rather than returned: a replacement that never arrives is a failed assertion and
	# not a fault that could not be applied, and the rounds after it are still worth running
	# against the five nodes that are left.
	await '(.nodes | length) == 6 and (.zones | length) == 3' "$recovery" \
		"the group replaced the stopped instance and it rejoined"

	# ---- Three. Every node of one zone held out of the membership, a zone at a time, round and round.
	for (( roll = 1; roll <= rolls; roll++ )); do
		for zone in "${zones[@]}"; do
			mapfile -t pair < <(zone_nodes "$zone")

			[ "${#pair[@]}" -gt 0 ] || continue

			echo "  Round three, pass $roll of $rolls: taking etcd from every node in $zone."

			if storm_isolate "every node in $zone lost etcd" "${pair[@]}"; then
				sleep "$hold"

				# Asked on the first pass alone. It is four walks of a store over Run Command,
				# which is a minute, and what it establishes — that this fault takes this zone's
				# copy out of the write path — is the same answer every pass round.
				[ "$roll" = 1 ] || { storm_rejoin; sleep "$between"; continue; }

				lagging=$(storm_lagging "$zone" "$(other_zone "$zone")") || lagging=

				if [ -z "$lagging" ]; then
					result 1 "both sides said what they hold while $zone was out of the membership"
				else
					expect_not "$lagging" 0 \
						"$zone fell behind the zones still taking writes while it was out"

					printf '  ---- %s of the %s keys written over and over differ across it\n' \
						"$lagging" "$hot_keys"
				fi
			fi

			storm_rejoin

			# Timed on the pass that measured the divergence alone.
			if [ "$roll" = 1 ]; then
				read -r behind caught <<< "$(storm_catchup "$zone")"

				if [ "$behind" = 0 ]; then
					result 0 "$zone caught up on what it missed once it was let back in"
					printf '  ---- it held the last acknowledged value of every key again %ss after the rule went\n' \
						"$caught"
				else
					result 1 "$zone caught up on what it missed once it was let back in — $behind of $hot_keys still behind after ${caught}s"
				fi
			fi

			sleep "$between"
		done

		storm_report "while every zone lost etcd in turn, pass $roll"
	done
}

# Every round takes its own fault away as it ends, so this is the run that died holding one. **The
# rule is cleared from every node and not only from the ones this run recorded**: a run that died
# between installing one and writing down where is a run whose record is short, and a node left
# unable to reach etcd is a node out of the membership for good.
heal()
{
	local stopped ids

	zone_heal

	stopped=$(aws ec2 describe-instances --instance-ids "$victim" \
		--query 'Reservations[].Instances[].State.Name' --output text 2> /dev/null)

	[ "$stopped" = stopped ] && aws ec2 start-instances --instance-ids "$victim" > /dev/null 2>&1

	ids=$(instances asyncdb | cut -f1)

	# shellcheck disable=SC2086
	[ -n "$ids" ] && blackhole_clear "$deaf_match" $ids

	deaf=()

	return 0
}

preflight()
{
	may_write_acls
	may_stop "$victim"

	# Run Command is how the rule gets onto an instance, and it is also how both sides of a cut
	# are asked what they hold — so an agent that does not answer takes the assertions with it as
	# well as the fault.
	# shellcheck disable=SC2086
	may_run $all_ids
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

expect_hot_settled "once the storm was over"

expect_hot_read "once the storm was over"

expect_storm_replicated "once the storm was over"

expect_round_trip 10 "a key written after the storm reads back what was written"

verdict
