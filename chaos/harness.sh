#! /usr/bin/env bash

# Sourced by every experiment in this folder.
#
# An experiment here is four things: a fault applied to the deployed stack with the AWS CLI,
# the assertions doc/runbook makes about what that fault looks like from outside, a client reading
# and writing throughout so that the fault lands on a cluster that is serving, and a verdict. This
# file owns all four, so that an experiment script is the fault and the assertions and nothing
# else.
#
# Everything is an environment variable, as in perf/. The defaults are the deployed stack:
#
#   CHAOS_STACK        the CloudFormation stack under test          asyncdb
#   CHAOS_URL          the address to drive, if not the Url output of the stack
#   CHAOS_TABLE        the table the suite seeds and reads          chaos
#   CHAOS_RECORDS      how many records it seeds                    200
#   CHAOS_SETTLE       how long a membership change is given        150 seconds
#   CHAOS_RECOVERY     how long an instance replacement is given    900 seconds
#   CHAOS_ONSET        how long a started fault is given to bite    20 seconds
#   CHAOS_CONVERGE     how long a resized cluster is given to move   300 seconds
#                      the records whose owner changed
#   CHAOS_LOAD         0 for an experiment against an idle cluster    1
#   CHAOS_LOAD_PAUSE   seconds between the load's writes              0.2

set -u

# Job control, so that stop_load signals the curl inside the load's own process group rather than
# orphaning it. perf/harness.sh does this for the same reason.
set -m

export LC_ALL=C

stack=${CHAOS_STACK:-asyncdb}
table=${CHAOS_TABLE:-chaos}
records=${CHAOS_RECORDS:-200}
settle=${CHAOS_SETTLE:-150}
recovery=${CHAOS_RECOVERY:-900}
onset=${CHAOS_ONSET:-20}
converge=${CHAOS_CONVERGE:-300}

# The lease is ten seconds and it is renewed every three, so a node that stops answering is out
# of the membership within one of them. Everything here that waits for a membership to change
# waits for CHAOS_SETTLE, which is that lease with the load balancer's own health check — thirty
# second interval, two failures — allowed for on top of it.
lease=10

work=$(mktemp -d)
remove_script=/tmp/asyncdb-chaos-remove
checks=0
failures=0
standing=0

# ---------------------------------------------------------------------------- the stack

setup()
{
	local missing

	for missing in aws jq curl; do
		command -v "$missing" > /dev/null || die "$missing is not installed."
	done

	aws sts get-caller-identity > /dev/null || die "No AWS credentials."

	url=${CHAOS_URL:-$(aws cloudformation describe-stacks --stack-name "$stack" \
		--query "Stacks[0].Outputs[?OutputKey=='Url'].OutputValue" --output text 2> /dev/null)}

	[ -n "${url:-}" ] && [ "$url" != None ] || die "The stack $stack has no Url output. Is it up?"

	base=$url/asyncdb

	# The instances are found by tag inside this stack's own VPC rather than by tag alone, so a
	# second stack in the account is never a target. Nothing else is taken from the template.
	vpc=$(aws cloudformation describe-stack-resource --stack-name "$stack" \
		--logical-resource-id VPC --query 'StackResourceDetail.PhysicalResourceId' --output text)

	echo "Stack $stack at $url, vpc $vpc."
}

die()
{
	echo "$*" >&2
	exit 1
}

# instances <name tag> — one line per running instance: id, availability zone, subnet.
instances()
{
	aws ec2 describe-instances \
		--filters "Name=tag:Name,Values=$1" \
			"Name=instance-state-name,Values=running" \
			"Name=vpc-id,Values=$vpc" \
		--query 'Reservations[].Instances[].[InstanceId,Placement.AvailabilityZone,SubnetId]' \
		--output text | sort
}

# Run Command is how a private instance is asked anything: the runner is outside the VPC and
# only the load balancer answers it, so a single node's own view of itself — which is the whole
# diagnostic in doc/runbook — is unreachable over HTTP. It is best effort by design: an instance
# that cannot be asked is reported and not asserted on.
ssm_run()
{
	local id=$1 command
	shift

	jq -n --arg c "$*" '{ commands: [ $c ] }' > "$work/command.json"

	command=$(aws ssm send-command --instance-ids "$id" \
		--document-name AWS-RunShellScript \
		--parameters "file://$work/command.json" \
		--query 'Command.CommandId' --output text 2> /dev/null) || return 1

	aws ssm wait command-executed --command-id "$command" --instance-id "$id" 2> /dev/null

	aws ssm get-command-invocation --command-id "$command" --instance-id "$id" \
		--query 'StandardOutputContent' --output text 2> /dev/null
}

# ssm_all <command> <instance>... — the same command on every instance in one send, with what each
# of them said left in $work/answer.<instance>. Non-zero when one of them said nothing at all,
# which is a node that could not be asked and never a node that answered nothing.
#
# ssm_run in a loop is the same command a wait apart, and a wait is most of a minute over six
# nodes: what one send buys is a fault that lands everywhere within a second or two of itself
# rather than a rolling one, and an answer from every node about one moment rather than six.
#
# It is every instance or none. send-command refuses a batch that names one instance it does not
# know, and taking that one out of the batch — which is right for a question, and every_node_whole
# does it — is wrong for a fault: a kill that quietly skipped a node is a weaker fault reported as
# the whole one.
ssm_all()
{
	local command=$1 id sent silent=0
	shift

	jq -n --arg c "$command" '{ commands: [ $c ] }' > "$work/command.json"

	rm -f "$work"/answer.*

	sent=$(aws ssm send-command --instance-ids "$@" \
		--document-name AWS-RunShellScript \
		--parameters "file://$work/command.json" \
		--query 'Command.CommandId' --output text 2> /dev/null) || return 1

	for id in "$@"; do
		aws ssm wait command-executed --command-id "$sent" --instance-id "$id" 2> /dev/null

		# --output text ends what it prints with a newline of its own, and what a node said ended
		# with one already, so the last byte is a line no node wrote — an empty answer to whatever
		# was asked, and a file that is not empty for a node that said nothing at all.
		aws ssm get-command-invocation --command-id "$sent" --instance-id "$id" \
			--query 'StandardOutputContent' --output text 2> /dev/null \
			| head -c -1 > "$work/answer.$id"

		[ -s "$work/answer.$id" ] || silent=$((silent + 1))
	done

	[ "$silent" = 0 ]
}

# The base image is the ECS-optimised AMI, which runs an agent container of its own, so the asyncdb
# container is the one whose image says asyncdb and never `docker ps -q | head -1`.
container_logs()
{
	echo 'docker logs $(docker ps --format "{{.ID}} {{.Image}}" | awk "/asyncdb/{print \$1; exit}") 2>&1'
}

# What one node says about itself, rather than what the load balancer happened to route to.
node_health()
{
	ssm_run "$1" 'curl -s --max-time 5 http://localhost:8080/health'
}

# node_status <instance> <path> — the status one node gives to a request made on the node itself.
# A fault that leaves some nodes answering and others not is one no assertion through the load
# balancer can make: which answer comes back then says as much about the routing as about the
# fault.
node_status()
{
	ssm_run "$1" "curl -s -o /dev/null --max-time 15 -w '%{http_code}' 'http://localhost:8080$2'"
}

# Whether every node holds what it owns. `incomplete` is a node's own state — a rebuild that did
# not read the whole of its share — and the load balancer answers from whichever node it picked,
# so a sampled health check is one that may never land on the node that is short. Every node is
# asked instead, and **a node that cannot be asked is a failed assertion and not a node that is
# whole**, which is the rule collect_holdings is written to for the same reason: a silent empty
# answer would say the opposite of what happened.
#
# One Run Command to the whole tier rather than one for each, because the wait is the cost and
# there is no reason to pay it six times. The agent is asked first for the same reason may_run
# asks it: send-command refuses the whole batch over one instance it does not know, so an
# instance whose agent is not answering is taken out of the batch and named here rather than
# costing the answer for the five that are.
every_node_whole()
{
	local ids online id answer command short= silent=

	ids=$(instances asyncdb | awk '{ print $1 }')

	[ -n "$ids" ] || { echo "No asyncdb instance is running." >&2; return 1; }

	online=$(aws ssm describe-instance-information \
		--filters "Key=InstanceIds,Values=$(echo "$ids" | paste -sd,)" \
		--query 'InstanceInformationList[?PingStatus == `Online`].InstanceId' \
		--output text 2> /dev/null)

	for id in $ids; do
		echo "$online" | tr '\t' '\n' | grep -qx "$id" || silent="$silent $id"
	done

	if [ -n "$online" ]; then
		jq -n --arg c 'curl -s --max-time 5 http://localhost:8080/health' \
			'{ commands: [ $c ] }' > "$work/command.json"

		# The ids are one argument each, which is what send-command takes.
		# shellcheck disable=SC2086
		command=$(aws ssm send-command --instance-ids $online \
			--document-name AWS-RunShellScript \
			--parameters "file://$work/command.json" \
			--query 'Command.CommandId' --output text 2> /dev/null) || return 1

		for id in $online; do
			aws ssm wait command-executed --command-id "$command" --instance-id "$id" 2> /dev/null

			answer=$(aws ssm get-command-invocation --command-id "$command" --instance-id "$id" \
				--query 'StandardOutputContent' --output text 2> /dev/null)

			if echo "$answer" | jq --exit-status 'has("incomplete")' > /dev/null 2>&1; then
				echo "$answer" | jq --exit-status '.incomplete | not' > /dev/null 2>&1 ||
					short="$short $id"
			else
				silent="$silent $id"
			fi
		done
	fi

	[ -z "$short" ] || echo "Holding less than they own:$short" >&2
	[ -z "$silent" ] || echo "Did not say whether they hold what they own:$silent" >&2

	[ -z "$short" ] && [ -z "$silent" ]
}

# ---------------------------------------------------------------------------- the data

seed()
{
	local i status

	# The seed is written every run and not only the first. A key whose owner changed hands moves
	# with it, but a key whose owner in every zone was terminated at once went with them and
	# nothing puts it back, so a run that took the last run's seed on trust would start each
	# experiment thinner than the one before it. Writing it again is two hundred idempotent
	# requests and makes runs repeatable.
	status=$(status --request PUT --header 'Content-Type: application/json' \
		--data '{}' "$base/table/$table")

	case $status in
		200 | 201) ;;
		*) die "Could not create $table: $status." ;;
	esac

	for (( i = 0; i < records; i++ )); do
		printf 'url = "%s"\noutput = "/dev/null"\n' "$base/table/$table/key/$i"
	done > "$work/seed"

	curl --silent --show-error --request PUT --data "chaos" \
		--header 'Content-Type: application/octet-stream' \
		--config "$work/seed" --write-out '%{http_code}\n' \
		| grep -cv '^2' > "$work/seeded"

	[ "$(cat "$work/seeded")" = 0 ] \
		|| die "$(cat "$work/seeded") of $records seed writes were refused."

	echo "Seeded $records records into $table."
}

status()
{
	curl --silent --output /dev/null --max-time 15 --write-out '%{http_code}' "$@"
}

# read_check <how many> — the number of reads of seeded records that did not answer 2xx. Every
# status is kept, because a count says an assertion failed and only the codes say how: a 404 is
# a copy that answered for a key it does not hold, a 000 is one that did not answer at all, and
# they are not the same incident.
read_check()
{
	local i failed=0 answer

	: > "$work/codes"

	for (( i = 0; i < $1; i++ )); do
		answer=$(status "$base/table/$table/key/$((i % records))")
		echo "$answer" >> "$work/codes"

		case $answer in
			2*) ;;
			*) failed=$((failed + 1)) ;;
		esac
	done

	echo "$failed"
}

# write_check <how many> — the same for writes, into keys of their own so that a failed write
# never leaves the seed short. Every write here is idempotent, so a retry is a retry.
write_check()
{
	local i failed=0 stamp=$RANDOM answer

	: > "$work/codes"

	for (( i = 0; i < $1; i++ )); do
		answer=$(status --request PUT --data 'chaos' \
			--header 'Content-Type: application/octet-stream' \
			"$base/table/$table/key/w-$stamp-$i")
		echo "$answer" >> "$work/codes"

		case $answer in
			2*) ;;
			*) failed=$((failed + 1)) ;;
		esac
	done

	echo "$failed"
}

# expect_readable <how many keys> <attempts each> <description> — every one of these records can
# still be read, which is not the same as every read being answered. A copy that is cut off but
# still in the load balancer answers for keys it does not hold, so a single miss says the request
# reached that node and not that the record is gone. What the runbook claims is that a record
# survives any one zone, and this is that claim: for each key, at least one of these attempts
# finds it.
expect_readable()
{
	local absent
	absent=$(readable "$1" "$2")

	if [ "$absent" = 0 ]; then
		result 0 "$3"
	else
		result 1 "$3 — $absent of $1 records could not be read in $2 attempts: $(codes)"
	fi
}

# readable <how many keys> <attempts each> — the same walk, answering how many of those records no
# attempt found. A resize is the fault that needs the number rather than the verdict: a key whose
# owner changed hands is a key the new owner does not hold, so what a resize leaves behind is
# measured and reported where a fault that breaks nothing permanent is asserted on.
readable()
{
	local key attempt found missing=0 answer

	: > "$work/codes"

	for (( key = 0; key < $1; key++ )); do
		found=0

		for (( attempt = 0; attempt < $2; attempt++ )); do
			answer=$(status "$base/table/$table/key/$((key % records))")
			echo "$answer" >> "$work/codes"

			case $answer in
				2*) found=1; break ;;
			esac
		done

		[ "$found" = 1 ] || missing=$((missing + 1))
	done

	echo "$missing"
}

# expect_codes <pattern> <description> — every status the check before this recorded matches. How a
# request was refused says as much as how many were: a key no copy holds is a 404 and a cluster that
# cannot answer at all is not, and a count of failures alone cannot tell them apart.
expect_codes()
{
	local unexpected
	unexpected=$(grep -cvE "$1" "$work/codes") || unexpected=0

	if [ "$unexpected" = 0 ]; then
		result 0 "$2"
	else
		result 1 "$2 — $unexpected answers were something else: $(codes)"
	fi
}

# expect_round_trip <how many> <description> — a key written now reads back what was written now.
# It is what a resized cluster has to be asked and a bare write does not: a write lands on the
# copies the membership names, and what a resize moves is which nodes those are.
expect_round_trip()
{
	local i failed=0 stamp=$RANDOM key code

	: > "$work/codes"

	for (( i = 0; i < $1; i++ )); do
		key=r-$stamp-$i

		code=$(status --request PUT --data "$stamp" \
			--header 'Content-Type: application/octet-stream' \
			"$base/table/$table/key/$key")
		echo "$code" >> "$work/codes"

		case $code in
			2*) ;;
			*) failed=$((failed + 1)); continue ;;
		esac

		code=$(read_value "$key")
		echo "$code" >> "$work/codes"

		case $code in
			2*) [ "$(cat "$work/value")" = "$stamp" ] || failed=$((failed + 1)) ;;
			*) failed=$((failed + 1)) ;;
		esac
	done

	if [ "$failed" = 0 ]; then
		result 0 "$2"
	else
		result 1 "$2 — $failed of $1 did not go there and come back: $(codes)"
	fi
}

# read_value <key> — the status, with the body left in $work/value. Everything else here reads a
# status alone, because every seeded record holds the same value; what a resize needs is which
# value came back.
read_value()
{
	curl --silent --max-time 15 --output "$work/value" --write-out '%{http_code}' \
		"$base/table/$table/key/$1"
}

# write_seed <value> — the seeded keys again with a value of the suite's own, so that a read after a
# resize says which copy answered rather than only that one did.
write_seed()
{
	local i failed=0 answer

	: > "$work/codes"

	for (( i = 0; i < records; i++ )); do
		answer=$(status --request PUT --data "$1" \
			--header 'Content-Type: application/octet-stream' \
			"$base/table/$table/key/$i")
		echo "$answer" >> "$work/codes"

		case $answer in
			2*) ;;
			*) failed=$((failed + 1)) ;;
		esac
	done

	echo "$failed"
}

# held <how many keys> <value> — three numbers: how many of those seeded keys answer that value, how
# many answer an older one, and how many no copy answers for at all. A copy that stopped being asked
# and started being asked again holds what it held when it stopped, and that is a stale value rather
# than a missing one — the same resize makes both, and they are not the same incident.
held()
{
	local i key code same=0 other=0 absent=0

	: > "$work/codes"

	for (( i = 0; i < $1; i++ )); do
		key=$((i % records))
		code=$(read_value "$key")
		echo "$code" >> "$work/codes"

		case $code in
			2*)
				if [ "$(cat "$work/value")" = "$2" ]; then
					same=$((same + 1))
				else
					other=$((other + 1))
				fi
				;;
			*) absent=$((absent + 1)) ;;
		esac
	done

	printf '%s %s %s\n' "$same" "$other" "$absent"
}

codes()
{
	sort "$work/codes" | uniq -c | sort -rn | awk '{ printf "%s×%s ", $1, $2 }'
}

# expect_reads / expect_writes <how many> <description> — the check and the evidence together. A
# bare count says an assertion failed and only the codes say how.
expect_reads()
{
	local failed
	failed=$(read_check "$1")

	if [ "$failed" = 0 ]; then
		result 0 "$2"
	else
		result 1 "$2 — $failed of $1 did not answer 2xx: $(codes)"
	fi
}

expect_writes()
{
	local failed
	failed=$(write_check "$1")

	if [ "$failed" = 0 ]; then
		result 0 "$2"
	else
		result 1 "$2 — $failed of $1 did not answer 2xx: $(codes)"
	fi
}

# refuse_writes <how many> <status> <description> — every write is refused, and with the one status
# that is the refusal meant. A count of failures is not the assertion: a write that did not answer
# at all, or one refused by a copy rather than for want of a leader, is a different incident and
# would pass a check that only counted the writes the store did not take.
refuse_writes()
{
	local refused
	write_check "$1" > /dev/null
	refused=$(grep -c "^$2$" "$work/codes")

	if [ "$refused" = "$1" ]; then
		result 0 "$3"
	else
		result 1 "$3 — $refused of $1 answered $2: $(codes)"
	fi
}

# await_writes <how many> <timeout> <description> — every copy of a key takes a write again. A
# write needs every copy, so this is the assertion that sees a node that has stopped answering its
# peers and is still renewing its lease: the membership await waits on says nothing about that,
# because a node in it is a node that renews and not a node that answers.
await_writes()
{
	local deadline=$((SECONDS + $2)) failed

	while :; do
		failed=$(write_check "$1")

		if [ "$failed" = 0 ]; then
			result 0 "$3"
			return 0
		fi

		[ "$SECONDS" -lt "$deadline" ] || break

		sleep 5
	done

	result 1 "$3 — $failed of $1 did not answer 2xx: $(codes)"
	return 1
}

scan_status()
{
	status "$base/table/$table/key?limit=100"
}

# ---------------------------------------------------------------------------- what a node holds

# The one question in this file that cannot be put through the load balancer, and the only way to
# see what a resize actually moved. A read through the load balancer is answered by whichever copy
# has the key — the owner, or another zone when the owner holds nothing — so it says a record
# exists somewhere and never where. A **scan carrying the forwarded header is answered where it
# lands**, which makes it that node's own share rather than its zone's merged answer: it is the
# request a rebuild makes of each node of a zone, asked here over Run Command.
#
# Two invariants follow from doc/database/cluster.md, and between them they are the whole of what a
# resize has to leave behind:
#
#   every zone holds the same keys        a zone holds a copy of the whole keyspace, so two zones
#                                         naming different keys is a copy that is short
#   no key is held twice inside a zone    a zone's nodes split the copy it holds, so a key in two
#                                         of their stores is a node that kept what it stopped owning
#
# A thousand is the largest page the API allows, and five of them is more keys than any experiment
# here writes — the cap is on the Run Commands rather than on the correctness.
holdings_limit=1000
holdings_pages=5

holdings_asked=0
holdings_total=0

# cluster::forwarded_header in src/cluster/cluster.h.
forwarded_header=X-Asyncdb-Forwarded

# holdings <instance> — the keys that node holds in its own store, sorted, one per line. Non-zero
# when the node could not be asked, which is not the same answer as a node that holds nothing.
holdings()
{
	local id=$1 from= url page keys pages=0

	: > "$work/keys"

	while [ "$pages" -lt "$holdings_pages" ]; do
		pages=$((pages + 1))

		# Every key this suite writes is unreserved in a URL, so the resume bound is not encoded.
		url="http://localhost:8080/table/$table/key?limit=$holdings_limit${from:+&from=$from}"

		page=$(ssm_run "$id" "curl -s --max-time 30 -H '$forwarded_header: true' '$url'")

		echo "$page" | jq --exit-status 'has("records")' > /dev/null 2>&1 || return 1

		keys=$(echo "$page" | jq -r '.records[].key')

		[ -n "$keys" ] || break

		echo "$keys" >> "$work/keys"

		# No cursor is a range that is exhausted. A bound is inclusive, so the next page begins
		# again with the key it resumed at, which the sort takes back out.
		echo "$page" | jq --exit-status 'has("next")' > /dev/null 2>&1 || break

		from=$(echo "$keys" | tail -1)
	done

	sort -u "$work/keys"
}

# collect_holdings — every node asked, into one file each named by the zone it is in. A node that
# cannot be asked is counted rather than passed over: a silent empty answer would make both
# assertions below say the opposite of what happened.
collect_holdings()
{
	local id zone

	holdings_asked=0
	holdings_total=0

	rm -rf "$work/holdings"
	mkdir -p "$work/holdings"

	while read -r id zone _; do
		holdings_total=$((holdings_total + 1))

		if holdings "$id" > "$work/holdings/$zone.$id"; then
			holdings_asked=$((holdings_asked + 1))
		fi
	done < <(instances asyncdb)

	[ "$holdings_asked" = "$holdings_total" ]
}

zones_held()
{
	ls "$work/holdings" | cut -d. -f1 | sort -u
}

# zones_differ — empty when every zone names the same keys as every other, and the zones that do not
# with how many keys apart they are otherwise.
zones_differ()
{
	local zone first= differ=

	for zone in $(zones_held); do
		cat "$work/holdings/$zone."* 2> /dev/null | sort -u > "$work/zone.$zone"

		if [ -z "$first" ]; then
			first=$zone
			continue
		fi

		cmp -s "$work/zone.$first" "$work/zone.$zone" \
			|| differ="$differ $zone($(comm -3 "$work/zone.$first" "$work/zone.$zone" | grep -c .))"
	done

	[ -z "$differ" ] || printf 'measured against %s:%s' "$first" "$differ"
}

# zone_duplicates — empty when no key is in two stores of one zone, and the zones that hold one
# twice with how many otherwise.
zone_duplicates()
{
	local zone duplicates worst=

	for zone in $(zones_held); do
		duplicates=$(cat "$work/holdings/$zone."* 2> /dev/null | sort | uniq -d | grep -c .)

		[ "$duplicates" = 0 ] || worst="$worst $zone($duplicates)"
	done

	[ -z "$worst" ] || printf '%s' "${worst# }"
}

# seed_held — how many of the seeded keys are in any store at all. What a terminated instance took
# with it is not something any mechanism in the cluster puts back — every copy of it went at once —
# so this is a number and never an assertion.
seed_held()
{
	local i

	for (( i = 0; i < records; i++ )); do
		echo "$i"
	done | sort > "$work/seed.keys"

	cat "$work/holdings"/* 2> /dev/null | sort -u > "$work/all.keys"

	comm -12 "$work/seed.keys" "$work/all.keys" | grep -c .
}

# expect_copies <when> — the two invariants, given time to arrive.
#
# Moving records because ownership moved is work in the background and not part of the update that
# caused it, so this waits for the cluster to converge the way await waits for a membership: what it
# asserts is that it gets there, and CHAOS_CONVERGE is how long it is given. It is asked after the
# shape has settled, so the seconds here are the mechanism's own and not the group's.
expect_copies()
{
	local deadline=$((SECONDS + converge)) differ duplicates

	while :; do
		collect_holdings
		differ=$(zones_differ)
		duplicates=$(zone_duplicates)

		[ -n "$differ" ] || [ -n "$duplicates" ] || break
		[ "$holdings_asked" = "$holdings_total" ] || break
		[ "$SECONDS" -lt "$deadline" ] || break

		sleep 15
	done

	expect "$holdings_asked" "$holdings_total" "every node said what it holds $1"

	if [ -z "$differ" ]; then
		result 0 "every zone holds the same keys $1"
	else
		result 1 "every zone holds the same keys $1 — $differ"
	fi

	if [ -z "$duplicates" ]; then
		result 0 "no key is held by two nodes of a zone $1"
	else
		result 1 "no key is held by two nodes of a zone $1 — held twice: $duplicates"
	fi

	printf '  ---- %s of %s seeded keys are held by some node %s\n' "$(seed_held)" "$records" "$1"
}

# ---------------------------------------------------------------------------- the load

# **A fault that lands on an idle cluster is not the fault anybody has.** Every experiment here
# keeps a client on the load balancer for the whole of itself — reads as fast as one connection
# answers them, and a write every CHAOS_LOAD_PAUSE seconds — so the node that is stopped, cut off,
# slowed or killed is one that was serving when it went, and what the cluster does next is
# measured while it is still being asked for things.
#
# The two halves are not there for the same reason. The **reads** are what a client saw, reported
# and never asserted on: they are of the seeded keys, every one of which exists, so a read that is
# not answered 2xx is the fault and never the key — and how many of them a fault costs is the load
# balancer's health check interval as much as the database. The **writes** are the assertion no
# error code can make: a write answered 2xx was taken by every copy of the key, so every one of
# them has to still be there when the fault is over. Each key is written once and never again,
# because a key written twice could read back either value with nothing wrong.
#
# **The load writes into a table of its own**, and that is not tidiness. A write that is *refused*
# may still have been taken by one copy — the copies of a write are written beside each other
# rather than in turn — and nothing here puts the rest of it back: there is no read repair, no
# anti-entropy, and a reconcile pass moves the records whose owner moved. So a load running into
# the seeded table would leave the zones holding different keys, which is what expect_copies
# asserts they do not. Measured on a two zone cluster killed twice over: twenty-seven keys apart,
# every one of them a write the client was told had failed, and no fewer three minutes later.
load_table=$table-load
load_stamp=
load_mark=0
reader=
writer=

start_load()
{
	local created

	[ "${CHAOS_LOAD:-1}" = 0 ] && { echo "CHAOS_LOAD is 0, so nothing is reading or writing."; return 0; }

	created=$(status --request PUT --header 'Content-Type: application/json' \
		--data '{}' "$base/table/$load_table")

	case $created in
		200 | 201) ;;
		*) die "Could not create $load_table: $created." ;;
	esac

	load_stamp=$RANDOM
	load_mark=0

	: > "$work/reads"
	: > "$work/written"

	# Both loops append a line at a time rather than holding one redirect open over the whole of
	# themselves, because load_report empties the reads as it goes: a truncated file under a
	# redirect that is still open is written at the offset it had reached, and the hole is zeroes.
	(
		while :; do
			printf '%s\n' "$(status "$base/table/$table/key/$((RANDOM % records))")" >> "$work/reads"
		done
	) &

	reader=$!

	(
		i=0

		while :; do
			i=$((i + 1))

			printf '%s %s\n' "$i" "$(status --request PUT --data "$load_stamp-$i" \
				--header 'Content-Type: application/octet-stream' \
				"$base/table/$load_table/key/load-$load_stamp-$i")" >> "$work/written"

			# A writer as fast as curl can be started is tens of thousands of keys to read back
			# over an experiment that waits for instances, and what this measures is whether a
			# write survived rather than how many of them there were.
			sleep "${CHAOS_LOAD_PAUSE:-0.2}"
		done
	) &

	writer=$!

	echo "A client is reading $table and writing $load_table throughout."
}

stop_load()
{
	local pid

	for pid in $reader $writer; do
		kill -- "-$pid" 2> /dev/null
		wait "$pid" 2> /dev/null
	done

	reader=
	writer=
}

# load_report <description> — what the load saw since the last report, which is what makes these
# lines a phase of the experiment rather than a running total: an experiment reports once while
# the fault is standing and again for what came after it.
#
# Reported and never asserted on, both halves. A read during the window in which the load balancer
# has not yet noticed a dead target is a read it sends to one, and a write refused while a copy of
# its key is missing is the design rather than a fault — what is asserted about the writes is
# expect_load_kept, and it is about the ones that were taken.
load_report()
{
	local reads answered writes taken lines

	[ -s "$work/reads" ] || [ -s "$work/written" ] || return 0

	reads=$(grep -c . "$work/reads") || reads=0
	answered=$(grep -c '^2' "$work/reads") || answered=0
	: > "$work/reads"

	lines=$(grep -c . "$work/written") || lines=0
	writes=$((lines - load_mark))
	taken=$(sed -n "$((load_mark + 1)),\$p" "$work/written" | grep -c ' 2[0-9][0-9]$') || taken=0
	load_mark=$lines

	printf '  ---- %s: %s of %s reads and %s of %s writes were answered 2xx\n' \
		"$1" "$answered" "$reads" "$taken" "$writes"
}

# load_survivors — four numbers over the writes the cluster acknowledged: how many there were, how
# many no copy answers for, how many answer something other than what was written, and how many of
# the **refused** writes are readable anyway.
#
# The readback is one curl over one connection rather than a request a process, because an
# experiment that waited for instances acknowledged thousands of them: a loop spawning curl a key
# at a time is minutes where this is seconds. Only what did not answer 2xx is asked again one at a
# time, because a read is answered by one copy and the load balancer picks which node is asked.
load_survivors()
{
	local n key code attempt taken lost=0 wrong=0 stray=0

	awk '$2 ~ /^2[0-9][0-9]$/ { print $1 }' "$work/written" | sort -n > "$work/acknowledged"
	awk '$2 !~ /^2[0-9][0-9]$/ { print $1 }' "$work/written" | sort -n > "$work/refused"

	taken=$(grep -c . "$work/acknowledged") || taken=0

	[ "$taken" = 0 ] && { printf '0 0 0 0\n'; return 0; }

	rm -rf "$work/back"
	mkdir -p "$work/back"

	while read -r n; do
		printf 'url = "%s"\noutput = "%s"\n' \
			"$base/table/$load_table/key/load-$load_stamp-$n" "$work/back/$n"
	done < "$work/acknowledged" > "$work/readback"

	curl --silent --max-time 30 --config "$work/readback" --write-out '%{http_code}\n' \
		> "$work/back.codes"

	paste "$work/acknowledged" "$work/back.codes" > "$work/back.status"

	awk '$2 !~ /^2[0-9][0-9]$/ { print $1 }' "$work/back.status" | sort > "$work/missed"

	# The value is the key's own number, so what a record should hold is worked out from the name
	# of the file it was read into. One pass rather than a process a key, for the same reason.
	awk -v stamp="$load_stamp" '
		FNR == 1 { key = FILENAME; sub(/.*\//, "", key); if ($0 != stamp "-" key) print key }' \
		"$work/back"/* | sort > "$work/unexpected"

	# A key that did not answer 2xx holds an error document rather than a value, so it is asked
	# again below and is not a value that changed.
	wrong=$(comm -23 "$work/unexpected" "$work/missed" | grep -c .) || wrong=0

	while read -r n; do
		key=load-$load_stamp-$n

		for attempt in 1 2 3; do
			code=$(read_value "$key")

			case $code in
				2*) break ;;
			esac
		done

		case $code in
			2*) [ "$(cat "$work/value")" = "$load_stamp-$n" ] || wrong=$((wrong + 1)) ;;
			*) lost=$((lost + 1)) ;;
		esac
	done < "$work/missed"

	# And the other side of a refused write, which is a measurement and never an assertion: the
	# copies of a write are written beside each other, so a write the client was told had failed is
	# one a copy may have taken. Nothing in the cluster puts the rest of its copies back.
	while read -r n; do
		case $(status "$base/table/$load_table/key/load-$load_stamp-$n") in
			2*) stray=$((stray + 1)) ;;
		esac
	done < <(head -200 "$work/refused")

	printf '%s %s %s %s\n' "$taken" "$lost" "$wrong" "$stray"
}

# expect_load_kept <description> — the load is stopped, what it last saw is reported, and **every
# write the cluster acknowledged is still there, holding what was written**. It is the claim the
# whole load exists to make, and it is an assertion for every fault that breaks nothing
# permanently.
expect_load_kept()
{
	local taken lost wrong stray

	stop_load
	load_report "$1"

	[ -s "$work/written" ] || return 0

	read -r taken lost wrong stray <<< "$(load_survivors)"

	expect_not "$taken" 0 "the cluster took writes while the fault was standing"
	expect "$lost" 0 "every write the cluster took $1 is still there"
	expect "$wrong" 0 "and every one of them reads back what was written"

	load_strays "$stray"
}

# report_load_kept <description> — the same, counted and never asserted on. It is for the three
# faults that **terminate** instances: a key whose owner in every zone went in the same update went
# with them, and nothing in the cluster puts that back — no rebuild of a copy, and no backup. What
# is asserted there instead is the shape of the answer, which expect_codes does.
report_load_kept()
{
	local taken lost wrong stray

	stop_load
	load_report "$1"

	[ -s "$work/written" ] || return 0

	read -r taken lost wrong stray <<< "$(load_survivors)"

	expect_not "$taken" 0 "the cluster took writes while the fault was standing"
	expect "$wrong" 0 "no write the cluster took $1 reads back as something else"

	printf '  ---- of %s writes the cluster took %s, %s are held by no copy afterwards\n' \
		"$taken" "$1" "$lost"

	load_strays "$stray"
}

# load_strays <how many of the refused writes were readable> — the other side of a refused write,
# and the reason the load has a table of its own. The copies of a write are written beside each
# other, so a write the client was told had failed is one a copy may have taken — and nothing here
# puts the rest of its copies back. Only the first two hundred are asked about, because this is a
# measurement and a fault that refuses thousands would otherwise be read back twice.
load_strays()
{
	local refused asked

	refused=$(grep -c . "$work/refused") || refused=0
	asked=$(( refused > 200 ? 200 : refused ))

	[ "$refused" = 0 ] && return 0

	if [ "$asked" = "$refused" ]; then
		printf '  ---- of %s writes that were refused, %s are readable anyway\n' "$refused" "$1"
	else
		printf '  ---- of %s writes that were refused, %s of the %s asked about are readable anyway\n' \
			"$refused" "$1" "$asked"
	fi
}

# ---------------------------------------------------------------------------- the assertions

result()
{
	checks=$((checks + 1))

	if [ "$1" = 0 ]; then
		printf '  PASS %s\n' "$2"
	else
		failures=$((failures + 1))
		printf '  FAIL %s\n' "$2"
	fi
}

# The document /health answers, whatever status it came with. A node that can order no write
# answers 503 so that the load balancer stops choosing it, and that is a node this suite still has
# to read a membership out of — which --fail would throw away along with the body. What --fail was
# keeping out is kept out by asking for a document instead: the nginx in front of the database
# answers its own error document, and anything that does not parse is nothing here.
health_document()
{
	curl --silent --max-time 10 "${1:-$base}/health" | jq --exit-status . 2> /dev/null
}

# Every sample has to match, and a sample that did not answer never does: the load balancer
# picks a different instance each time, so a predicate that holds for eight samples is one that
# holds for the cluster rather than for whichever node answered first.
health_matches()
{
	local i answer

	for (( i = 0; i < ${2:-8}; i++ )); do
		answer=$(health_document) || return 1
		echo "$answer" | jq --exit-status "$1" > /dev/null 2>&1 || return 1
	done
}

# The tier as /health names it: how many nodes, how many zones, and how many nodes a zone holds.
# All three and not the first alone, because six nodes in three zones and nine nodes in three zones
# are the same thing to anything that counts zones — and the third number is what says the group
# balanced them over the subnets, which is the whole of how a copy is split.
shape()
{
	health_document \
		| jq -c '[ (.nodes | length), (.zones | length), ([ .zones[] | length ] | unique) ]'
}

# What the auto scaling groups have tried lately, reported and never asserted on. A membership
# that never arrived and an instance that was never launched read the same from outside, and this
# is the only thing that tells them apart: /health says the shape the cluster has and the group
# says the shape it wants, and neither of them says why the two differ. A launch the account had
# no room for is a failed activity here and nothing at all anywhere else.
scaling_activities()
{
	local logical group

	for logical in AutoScalingGroup EtcdAutoScalingGroup; do
		group=$(aws cloudformation describe-stack-resource --stack-name "$stack" \
			--logical-resource-id "$logical" \
			--query 'StackResourceDetail.PhysicalResourceId' --output text 2> /dev/null)

		[ -n "$group" ] && [ "$group" != None ] || continue

		aws autoscaling describe-scaling-activities --auto-scaling-group-name "$group" \
			--query 'Activities[:5].[StatusCode,StartTime,StatusMessage,Description]' \
			--output text 2> /dev/null | sed "s/^/  ---- $logical /"
	done
}

# await <jq predicate> <timeout> <description> — the cluster reaching a state, or not.
await()
{
	local deadline=$((SECONDS + $2))

	while [ "$SECONDS" -lt "$deadline" ]; do
		# The load balancer picks a different instance each time and there is no stickiness, so
		# a predicate that holds for one run of samples can hold because the run was lucky
		# rather than because the cluster settled — a node in a state this is waiting to see
		# the end of is a node the samples can miss. Asking again after a pause, and needing
		# both, is what tells a settled cluster from a lucky sample.
		if health_matches "$1" 12 && sleep 8 && health_matches "$1" 12; then
			result 0 "$3"
			return 0
		fi
		sleep 5
	done

	# A predicate that was never satisfied and an address that stopped answering are not the
	# same failure, and reporting the second as the first is how a stack deleted underneath a
	# run reads as sixty broken assertions.
	local last
	last=$(health_document)

	if [ -z "$last" ]; then
		result 1 "$3 — $base/health did not answer at all"
	else
		result 1 "$3 — health says $(echo "$last" \
			| jq -c '{nodes: (.nodes | length), zones: (.zones | length), leads}' 2> /dev/null)"
	fi

	# A shape this waited for is a shape instances have to arrive for, so what the groups tried
	# is part of the failure and not a separate investigation.
	scaling_activities

	return 1
}

# await_node <instance> <jq predicate> <timeout> <description> — the same, of one node over Run
# Command rather than of the load balancer. It is what a fault that cuts nodes off needs: nothing
# takes an isolated node out of the load balancer until its own health check has failed twice, so
# until then a sample of /health through it is as likely to be the isolated side's answer as the
# majority's — and a predicate about the membership then holds only when the sampling was lucky.
# Asking a node that is on the side of the fault this is making a claim about needs no luck.
await_node()
{
	local deadline=$((SECONDS + $3)) answer last=

	while [ "$SECONDS" -lt "$deadline" ]; do
		answer=$(node_health "$1")
		[ -n "$answer" ] && last=$answer

		if echo "$answer" | jq --exit-status "$2" > /dev/null 2>&1; then
			result 0 "$4"
			return 0
		fi

		sleep 5
	done

	if [ -z "$last" ]; then
		result 1 "$4 — $1 could not be asked at all"
	else
		result 1 "$4 — $1 says $(echo "$last" \
			| jq -c '{nodes: (.nodes | length), zones: (.zones | length), leads}' 2> /dev/null)"
	fi

	return 1
}

# The opposite: a state that must hold for a while rather than one that must arrive. It is what
# says a node stayed in the membership while it was slow, which is the whole of "up but wrong".
holds()
{
	local deadline=$((SECONDS + $2))

	while [ "$SECONDS" -lt "$deadline" ]; do
		if ! health_matches "$1" 4; then
			result 1 "$3"
			return 1
		fi
		sleep 5
	done

	result 0 "$3"
}

expect()
{
	if [ "$1" = "$2" ]; then
		result 0 "$3"
	else
		result 1 "$3 — expected $2, got $1"
	fi
}

expect_not()
{
	if [ "$1" != "$2" ]; then
		result 0 "$3"
	else
		result 1 "$3 — got $1"
	fi
}

# ---------------------------------------------------------------------------- the fault

# An experiment declares its fault as three functions, and this file owns when they run:
#
#   inject     applies the fault, and returns non-zero if it could not be applied
#   heal       takes it away, and is safe to run twice or against a fault that never landed
#   preflight  asks whether inject would work, without applying anything — chaos/validate.sh
#
# Every fault here is applied with the AWS CLI directly. Nothing is passed to a service that
# would apply it on the suite's behalf, and nothing but heal takes one away, which is why heal
# is called by the exit trap as well as by the experiment: a run that dies holding a fault is
# a run that still removes it.

# fault_start — apply the fault, or say why not.
fault_start()
{
	# Validating is asking every question an experiment asks of the account and applying
	# nothing: the targets are resolved, the permissions are dry run, and the script stops
	# here. chaos/validate.sh is this mode over every experiment.
	if [ "${CHAOS_VALIDATE:-0}" = 1 ]; then
		preflight
		verdict
		exit $?
	fi

	# Standing before injected, not after. An inject that fails halfway has applied half a
	# fault, and the half it applied is the half heal has to take away.
	standing=1

	if ! inject; then
		result 1 "the fault was injected"
		return 1
	fi

	result 0 "the fault was injected"

	# An assertion made before the fault has landed is an assertion about nothing.
	sleep "$onset"
}

# fault_stop — the assertions are done, so take the fault away rather than sit and watch its
# own timer expire. Every experiment that calls this follows it with the recovery it asserts on,
# which is the check that the fault actually went: none of it is taken on trust.
fault_stop()
{
	[ "$standing" = 1 ] || return 0
	standing=0

	echo "  The assertions are done. Taking the fault away."
	heal
}

# ---------------------------------------------------------------------------- faults over SSM

# fault_script <seconds> <install> <remove> — the body of the AWS-RunShellScript command that
# carries a fault onto an instance: install it, hold it, take it away.
#
# The detached timer is the only thing standing between a run that died and an instance left
# holding a fault, because a cancelled Run Command runs no trap in the script it cancelled. It
# is later than the fault itself and removing a fault that is already gone is nothing, so it
# costs a run that ends properly nothing at all.
#
# **What ends a fault is fault_stop and not this script.** The timers here are minutes long: an
# experiment that waited one out would leave every experiment after it measuring this fault.
fault_script()
{
	local seconds=$1 install=$2 remove=$3

	# The remove is written down before anything is installed and run from there by both the
	# trap and the timer. Nesting it inside either as text is what breaks on the first quote it
	# contains, and every fault here removes itself with a command substitution.
	cat <<-SCRIPT
	set -o errexit

	cat > $remove_script <<'REMOVE'
	$remove
	REMOVE

	setsid nohup bash -c "sleep $((seconds + 120)); bash $remove_script" > /dev/null 2>&1 &

	trap 'bash $remove_script; exit 0' INT TERM

	$install
	echo injected

	sleep $seconds

	bash $remove_script
	echo removed
	SCRIPT
}

# fault_send <seconds> <install> <remove> <instance>... — one command to every instance at once,
# and never waited for: the script it runs holds the fault for as long as the assertions need,
# so a call that waited for it would outlast the experiment.
fault_send()
{
	local seconds=$1 install=$2 remove=$3
	shift 3

	fault_script "$seconds" "$install" "$remove" \
		| jq -Rs '{ commands: [ . ] }' > "$work/parameters.json"

	aws ssm send-command --instance-ids "$@" \
		--document-name AWS-RunShellScript \
		--parameters "file://$work/parameters.json" \
		--query 'Command.CommandId' --output text
}

# fault_await <command> <instance>... — every invocation has reached its instance and is running
# the script. A send that answered is a command the service accepted and not a fault that landed:
# an instance the agent has not registered leaves an invocation pending until it times out.
fault_await()
{
	local command=$1 deadline=$((SECONDS + 300)) wanted=$(($# - 1)) statuses

	while [ "$SECONDS" -lt "$deadline" ]; do
		statuses=$(aws ssm list-command-invocations --command-id "$command" \
			--query 'CommandInvocations[].Status' --output text 2> /dev/null)

		case $statuses in
			*Failed* | *TimedOut* | *Cancelled*)
				echo "  Command $command is $statuses." >&2
				return 1
				;;
		esac

		# The invocations appear one at a time, so a count that is short is a list that
		# is not finished rather than an instance that refused.
		case $statuses in
			*Pending* | *Delayed* | '') ;;
			*)
				[ "$(printf '%s' "$statuses" | wc -w)" = "$wanted" ] && return 0
				;;
		esac

		sleep 5
	done

	echo "  Command $command is still ${statuses:-unreported} after five minutes." >&2
	return 1
}

# ---------------------------------------------------------------------------- a deaf node

# The rule the two experiments that need a node to stop answering install, and it is a rule of
# our own rather than a document that blackholes a port, because asyncdb is a container behind a
# published port: everything a peer sends it is DNATed and forwarded, so it goes through FORWARD
# and never INPUT, and everything the container sends is forwarded too and never OUTPUT. A rule
# in INPUT or OUTPUT blocks nothing here. DOCKER-USER is the chain docker leaves in FORWARD for
# exactly this.
#
# It rejects rather than drops. A node waits thirty seconds on another node
# (cluster::config::timeout_seconds), so a dropped packet is a node that hangs, and what these two
# are about is a node that does not answer: a reset says so at once, the copy that does answer is
# asked next, and node-latency is the experiment about waiting.
blackhole_rule()
{
	printf '%s -j REJECT --reject-with tcp-reset' "$1"
}

blackhole_remove()
{
	printf 'iptables -D DOCKER-USER %s 2> /dev/null || true' "$(blackhole_rule "$1")"
}

# blackhole <seconds> <iptables match> <instance>... — install it, and wait until it is installed.
blackhole()
{
	local seconds=$1 match=$2 command
	shift 2

	command=$(fault_send "$seconds" \
		"iptables -I DOCKER-USER $(blackhole_rule "$match")" \
		"$(blackhole_remove "$match")" "$@") || return 1

	fault_await "$command" "$@"
}

# blackhole_clear <iptables match> <instance>... — take the rule out over Run Command, which is
# how this fault is actually ended: the script that installed it is sleeping, and nothing but its
# own timer would make it stop. Deleting a rule that is not there is nothing, so this is right
# whatever became of that script.
#
# The agent answers a node that is deaf to its peers: a request the host makes to a published port
# is translated on its way out of the host and never crosses FORWARD, which is where the rule is.
blackhole_clear()
{
	local match=$1 rule delete check id answered
	shift

	rule=$(blackhole_rule "$match")

	# Breaking out of a delete that failed matters more than it looks: a loop that cannot delete
	# what it can still find is a Run Command that never returns.
	delete="while iptables -C DOCKER-USER $rule 2> /dev/null; do"
	delete="$delete iptables -D DOCKER-USER $rule || break; done"

	# The delete is not trusted either. What comes back is what the chain says afterwards.
	check="iptables -C DOCKER-USER $rule 2> /dev/null && echo present || echo cleared"

	for id in "$@"; do
		answered=$(ssm_run "$id" "$delete; $check")

		[ "${answered:-}" = cleared ] \
			|| echo "  $id is still holding the rule — ${answered:-it could not be asked}"
	done

	return 0
}

# ---------------------------------------------------------------------------- a slow node

# The device is read out of the default route rather than named: an instance of this generation
# is ens5 and not eth0. A root qdisc on it delays what the container sends for the same reason
# DOCKER-USER sees what the container sends — everything the host forwards leaves through the
# device the route names — and tc is not on the image, so it comes from the distribution
# repositories, which is what makes this fault depend on the private subnets' route out.
latency_remove()
{
	printf 'tc qdisc del dev $(ip route show default | cut -d" " -f5) root 2> /dev/null || true'
}

# latency <seconds> <milliseconds> <jitter> <cidr> <instance> — everything the node says to the
# cidr is delayed. Only egress is touched: a delay in each direction is twice the delay, and one
# direction is enough to make a node slow.
latency()
{
	local seconds=$1 delay=$2 jitter=$3 cidr=$4 instance=$5 command install

	install=$(cat <<-INSTALL
	dnf install --assumeyes --quiet iproute-tc > /dev/null 2>&1 || true

	dev=\$(ip route show default | cut -d" " -f5)

	tc qdisc add dev \$dev root handle 1: prio
	tc qdisc add dev \$dev parent 1:3 handle 30: netem delay ${delay}ms ${jitter}ms
	tc filter add dev \$dev protocol ip parent 1:0 prio 3 u32 match ip dst $cidr flowid 1:3
	INSTALL
	)

	command=$(fault_send "$seconds" "$install" "$(latency_remove)" "$instance") || return 1
	fault_await "$command" "$instance"
}

latency_clear()
{
	local id answered check

	check='tc qdisc show dev $(ip route show default | cut -d" " -f5)'
	check="$check | grep -q netem && echo present || echo cleared"

	for id in "$@"; do
		answered=$(ssm_run "$id" "$(latency_remove); $check")

		[ "${answered:-}" = cleared ] \
			|| echo "  $id is still holding the qdisc — ${answered:-it could not be asked}"
	done

	return 0
}

# ---------------------------------------------------------------------------- a full disk

# The percentage is of the disk and not of what is free on it, so only 100 fills what is free.
#
# fallocate takes the space without writing it, so thirty gigabytes go in a moment rather than in
# the minutes a copy of them would take — and a fault that takes minutes to land is one the
# assertions reach before it has. What fallocate cannot take is the reserve the filesystem keeps
# back, which is megabytes, so a hundred per cent writes until the write fails as well: anything
# short of a full volume is not this failure at all.
fill_file=/var/asyncdb-chaos.fill

fill_remove()
{
	printf 'rm -f %s %s.rest || true' "$fill_file" "$fill_file"
}

# fill <seconds> <percent> <instance>
fill()
{
	local seconds=$1 percent=$2 instance=$3 command install

	install=$(cat <<-INSTALL
	total=\$(df --block-size=1 --output=size / | tail -1)
	used=\$(df --block-size=1 --output=used / | tail -1)
	avail=\$(df --block-size=1 --output=avail / | tail -1)

	want=\$(( total * $percent / 100 - used ))
	[ "\$want" -gt "\$avail" ] && want=\$avail

	if [ "\$want" -gt 0 ]; then
		fallocate --length "\$want" $fill_file 2> /dev/null || true
	fi

	if [ "$percent" -ge 100 ]; then
		dd if=/dev/zero of=$fill_file.rest bs=1M 2> /dev/null || true
	fi
	INSTALL
	)

	command=$(fault_send "$seconds" "$install" "$(fill_remove)" "$instance") || return 1
	fault_await "$command" "$instance"
}

fill_clear()
{
	local id answered check

	check="{ [ -e $fill_file ] || [ -e $fill_file.rest ]; }"
	check="$check && echo present || echo cleared"

	for id in "$@"; do
		answered=$(ssm_run "$id" "$(fill_remove); $check")

		[ "${answered:-}" = cleared ] \
			|| echo "  $id is still holding the fill — ${answered:-it could not be asked}"
	done

	return 0
}

# ---------------------------------------------------------------------------- a zone cut off

# zone_cut <subnet> <zone> — a network acl of the suite's own on that subnet, denying every other
# zone's subnets and allowing what is left. That is the whole of "this zone cannot see the
# others", and it is the one fault here that is not a call on an instance: nothing is asked of
# the instances, so it works on a stack whose instances cannot be reached at all.
#
# What it replaced is written down before anything is replaced, because putting the association
# back is the only way this fault goes away.
zone_cut()
{
	local subnet=$1 zone=$2 acl rule=100 cidr others association original new

	# Every other zone's subnets, public and private alike: the load balancer is in the public
	# ones and the peers and etcd in the private.
	others=$(aws ec2 describe-subnets --filters "Name=vpc-id,Values=$vpc" \
		--query "Subnets[?AvailabilityZone!='$zone'].CidrBlock" --output text) || return 1

	[ -n "$others" ] || { echo "  No subnets outside $zone to cut off." >&2; return 1; }

	association=$(aws ec2 describe-network-acls \
		--filters "Name=association.subnet-id,Values=$subnet" \
		--query "NetworkAcls[].Associations[?SubnetId=='$subnet'].NetworkAclAssociationId" \
		--output text) || return 1

	original=$(aws ec2 describe-network-acls \
		--filters "Name=association.subnet-id,Values=$subnet" \
		--query "NetworkAcls[?Associations[?SubnetId=='$subnet']].NetworkAclId" \
		--output text) || return 1

	acl=$(aws ec2 create-network-acl --vpc-id "$vpc" \
		--tag-specifications \
			'ResourceType=network-acl,Tags=[{Key=Name,Value=asyncdb-chaos}]' \
		--query 'NetworkAcl.NetworkAclId' --output text) || return 1

	echo "$acl" > "$work/acl"

	# A network acl is stateless and the lowest matching rule number wins, so the denials come
	# first and the allow of everything else follows them. Nothing is implied: an acl denies
	# what no rule of its own matched, so the allows have to be written.
	for cidr in $others; do
		aws ec2 create-network-acl-entry --network-acl-id "$acl" --rule-number "$rule" \
			--protocol -1 --rule-action deny --cidr-block "$cidr" --ingress || return 1
		aws ec2 create-network-acl-entry --network-acl-id "$acl" --rule-number "$rule" \
			--protocol -1 --rule-action deny --cidr-block "$cidr" --egress || return 1

		rule=$((rule + 10))
	done

	aws ec2 create-network-acl-entry --network-acl-id "$acl" --rule-number 900 \
		--protocol -1 --rule-action allow --cidr-block 0.0.0.0/0 --ingress || return 1
	aws ec2 create-network-acl-entry --network-acl-id "$acl" --rule-number 900 \
		--protocol -1 --rule-action allow --cidr-block 0.0.0.0/0 --egress || return 1

	# IPv6 is left alone, deliberately. A node addresses its peers and etcd by the private IPv4
	# the user data read out of IMDS, so the cut is complete without it — and the instances keep
	# the route out that Systems Manager answers on, which is what lets the isolated side still
	# be asked what it thinks of itself while it is cut off.
	aws ec2 create-network-acl-entry --network-acl-id "$acl" --rule-number 910 \
		--protocol -1 --rule-action allow --ipv6-cidr-block ::/0 --ingress || return 1
	aws ec2 create-network-acl-entry --network-acl-id "$acl" --rule-number 910 \
		--protocol -1 --rule-action allow --ipv6-cidr-block ::/0 --egress || return 1

	new=$(aws ec2 replace-network-acl-association --association-id "$association" \
		--network-acl-id "$acl" --query NewAssociationId --output text) || return 1

	printf '%s %s\n' "$new" "$original" > "$work/association"
}

# The association goes back first and the acl goes after it: an acl a subnet is still associated
# with cannot be deleted. Deleting it is best effort — one left behind is tagged asyncdb-chaos
# and associated with nothing — but putting the association back is not, and it is the recovery
# assertion that says whether it worked.
zone_heal()
{
	local new original

	if [ -s "$work/association" ]; then
		read -r new original < "$work/association"

		aws ec2 replace-network-acl-association --association-id "$new" \
			--network-acl-id "$original" > /dev/null 2>&1

		: > "$work/association"
	fi

	if [ -s "$work/acl" ]; then
		aws ec2 delete-network-acl --network-acl-id "$(cat "$work/acl")" > /dev/null 2>&1

		: > "$work/acl"
	fi

	return 0
}

# ---------------------------------------------------------------------------- a resized tier

# The one fault here that is applied by asking for it rather than by breaking something: the
# database tier's shape is two parameters of the stack, Nodes and Zones, and moving either is a
# stack update. Zones is how many copies of the keyspace there are, Nodes is how many ways a zone
# splits the copy it holds, and the auto scaling group does the rest — it balances what it is given
# over the subnets it spans, and an instance reads its own zone out of IMDS.
#
# Nothing about it is gentler than a fault an experiment injects. The records whose owner moves with
# the membership are moved after it, on a pass of each node's own, and a node that is terminated
# takes what only it held with it: what a resize costs is whatever went in the same update as every
# copy of it.

# stack_parameters <key=value>... — the parameter list for an update: these, and every other
# parameter of the stack as it stands. UsePreviousValue keeps an SSM-typed parameter's name rather
# than the tag it resolved to, so a resize deploys the version already running.
stack_parameters()
{
	local name value override

	for name in $(aws cloudformation describe-stacks --stack-name "$stack" \
		--query 'Stacks[0].Parameters[].ParameterKey' --output text); do
		value=

		for override in "$@"; do
			[ "${override%%=*}" = "$name" ] && value=${override#*=}
		done

		if [ -n "$value" ]; then
			printf 'ParameterKey=%s,ParameterValue=%s\n' "$name" "$value"
		else
			printf 'ParameterKey=%s,UsePreviousValue=true\n' "$name"
		fi
	done
}

# stack_update <key=value>... — the update, waited out. The template is the one deployed and not the
# one checked out: what is under test is the stack the pipeline stood up, and carrying this
# checkout's template would be a second change nobody asked for.
stack_update()
{
	local parameters answer

	mapfile -t parameters < <(stack_parameters "$@")

	[ "${#parameters[@]}" -gt 0 ] || { echo "  The stack $stack names no parameters." >&2; return 1; }

	# An update that would change nothing is refused rather than applied, and that is the state it
	# was asking for — which is what makes heal safe to run twice, or against a fault that never
	# landed.
	answer=$(aws cloudformation update-stack --stack-name "$stack" \
		--use-previous-template --capabilities CAPABILITY_NAMED_IAM \
		--parameters "${parameters[@]}" 2>&1) || {
			case $answer in
				*"No updates are to be performed"*) return 0 ;;
			esac

			echo "  $answer" >&2
			return 1
		}

	# The wait is over the stack and says nothing about the instances: an auto scaling group is
	# updated the moment the group takes the new capacity, and what it then launches or terminates
	# takes minutes more. Every assertion about the shape of the cluster is made against /health.
	aws cloudformation wait stack-update-complete --stack-name "$stack" 2> /dev/null
}

# ---------------------------------------------------------------------------- the dry runs

# What an experiment's preflight is made of. Each one answers a question a full run would take
# minutes to reach, applies nothing, and costs one API call.

# may_stop <instance>... — the dry run EC2 offers. It stops nothing and answers whether these
# credentials could.
may_stop()
{
	local answer
	answer=$(aws ec2 stop-instances --dry-run --instance-ids "$@" 2>&1)

	case $answer in
		*DryRunOperation*) result 0 "these credentials may stop $*" ;;
		*) result 1 "these credentials may stop $* — $answer" ;;
	esac
}

# may_write_acls — the same, for the one fault that writes a network acl.
may_write_acls()
{
	local answer
	answer=$(aws ec2 create-network-acl --dry-run --vpc-id "$vpc" 2>&1)

	case $answer in
		*DryRunOperation*) result 0 "these credentials may write a network acl in $vpc" ;;
		*) result 1 "these credentials may write a network acl in $vpc — $answer" ;;
	esac
}

# may_run <instance>... — Run Command has no dry run, and what actually fails is an instance
# whose agent never registered, so this asks whether Systems Manager can see them at all.
may_run()
{
	local online

	online=$(aws ssm describe-instance-information \
		--filters "Key=InstanceIds,Values=$(IFS=,; echo "$*")" \
		--query 'length(InstanceInformationList[?PingStatus == `Online`])' \
		--output text 2> /dev/null)

	expect "${online:-0}" "$#" "the SSM agent answers on $*"
}

# may_resize <key=value>... — the dry run CloudFormation offers, which is a change set: it is the
# update, worked out and written down, and it applies nothing until it is told to. This deletes it
# instead.
#
# It answers the question a resize has to ask before it waits ten minutes to find out — whether the
# stack standing is one whose template takes these parameters at all, which a stack created before
# it did is not — and it answers it against the deployed template rather than the checkout.
may_resize()
{
	local parameters name set status changed

	mapfile -t parameters < <(stack_parameters "$@")

	name=chaos-$$-$RANDOM

	set=$(aws cloudformation create-change-set --stack-name "$stack" \
		--change-set-name "$name" --use-previous-template \
		--capabilities CAPABILITY_NAMED_IAM \
		--parameters "${parameters[@]}" --query Id --output text 2>&1) || {
			result 1 "$stack takes $* — $set"
			return 1
		}

	# A change set that could not be worked out fails rather than erroring, and the reason is the
	# whole diagnosis: a parameter the template does not have and a value it does not allow both
	# land here.
	aws cloudformation wait change-set-create-complete --change-set-name "$set" 2> /dev/null

	status=$(aws cloudformation describe-change-set --change-set-name "$set" \
		--query 'join(`: `, [Status, StatusReason || `no reason given`])' --output text)

	changed=$(aws cloudformation describe-change-set --change-set-name "$set" \
		--query 'join(`,`, Changes[].ResourceChange.LogicalResourceId)' --output text)

	aws cloudformation delete-change-set --change-set-name "$set" > /dev/null 2>&1

	case $status in
		CREATE_COMPLETE*) result 0 "$stack would take $* — it changes ${changed:-nothing}" ;;
		*) result 1 "$stack would take $* — the change set says $status" ;;
	esac
}

# may_suspend <group> — the one autoscaling call any experiment makes, which has no dry run
# either. Reading the group is what says the name resolves and the credentials reach the service.
may_suspend()
{
	local answer
	answer=$(aws autoscaling describe-auto-scaling-groups --auto-scaling-group-names "$1" \
		--query 'AutoScalingGroups[0].AutoScalingGroupName' --output text 2>&1)

	expect "$answer" "$1" "the group $1 can be read, and so suspended"
}

# Breaking nothing, and asking the two questions every experiment here needs answered before one
# of them has waited minutes to find out: whether these credentials may stop an instance, and
# whether Run Command reaches one.
preflight_chaos()
{
	local instance answer

	instance=$(instances asyncdb | cut -f1 | head -1)

	[ -n "$instance" ] || { echo "No asyncdb instances in $vpc." >&2; return 1; }

	answer=$(aws ec2 stop-instances --dry-run --instance-ids "$instance" 2>&1)

	case $answer in
		*DryRunOperation*) ;;
		*)
			printf '%s\n' "$answer" | sed 's/^/  /' >&2
			echo >&2
			echo "The operator policy is part of chaos.yaml. If the chaos stack is not" >&2
			echo "standing, 'make create-chaos-stack' attaches it, and it goes away with" >&2
			echo "the stack." >&2
			return 1
			;;
	esac

	answer=$(aws ssm describe-instance-information \
		--filters "Key=InstanceIds,Values=$instance" \
		--query 'InstanceInformationList[0].PingStatus' --output text 2> /dev/null)

	[ "$answer" = Online ] || {
		echo "The SSM agent does not answer on $instance — it says ${answer:-nothing}." >&2
		echo "Four of the seven experiments inject through it." >&2
		return 1
	}
}

# ---------------------------------------------------------------------------- the verdict

cleanup()
{
	local status=$?

	stop_load

	# The fault is taken away here as well as by the experiment, because an experiment that died
	# holding one never reached its own fault_stop. It is a no-op when the fault has already
	# gone, and $work is still standing, which is where a fault's own record of what it replaced
	# is kept.
	fault_stop

	rm -rf "$work"

	return $status
}

trap cleanup EXIT INT TERM

verdict()
{
	echo
	if [ "$failures" -gt 0 ]; then
		printf '%s of %s checks failed.\n' "$failures" "$checks"
		return 1
	fi

	printf 'All %s checks passed.\n' "$checks"
}

banner()
{
	echo
	echo "== $1"
	echo "   $2"
	echo
}
