#! /usr/bin/env bash

# Sourced by every experiment in this folder.
#
# An experiment here is four things: a fault the AWS Fault Injection Service applies to the
# deployed stack, the assertions doc/runbook makes about what that fault looks like from
# outside, a probe recording what a client saw while it ran, and a verdict. This file owns all
# four, so that an experiment script is the fault and the assertions and nothing else.
#
# Everything is an environment variable, as in perf/. The defaults are the deployed stack:
#
#   CHAOS_STACK        the CloudFormation stack under test          asyncdb
#   CHAOS_ROLE_STACK   the stack holding the FIS role               asyncdb-chaos
#   CHAOS_ROLE         the role arn, if not taken from that stack
#   CHAOS_URL          the address to drive, if not the Url output of the stack
#   CHAOS_TABLE        the table the suite seeds and reads          chaos
#   CHAOS_RECORDS      how many records it seeds                    200
#   CHAOS_SETTLE       how long a membership change is given        150 seconds
#   CHAOS_RECOVERY     how long an instance replacement is given    900 seconds
#   CHAOS_ONSET        how long a started fault is given to bite    20 seconds

set -u

# Job control, so that stop_probe signals the curl inside the probe's own process group rather
# than orphaning it. perf/harness.sh does this for the same reason.
set -m

export LC_ALL=C

stack=${CHAOS_STACK:-asyncdb}
role_stack=${CHAOS_ROLE_STACK:-asyncdb-chaos}
table=${CHAOS_TABLE:-chaos}
records=${CHAOS_RECORDS:-200}
settle=${CHAOS_SETTLE:-150}
recovery=${CHAOS_RECOVERY:-900}
onset=${CHAOS_ONSET:-20}

# The lease is ten seconds and it is renewed every three, so a node that stops answering is out
# of the membership within one of them. Everything here that waits for a membership to change
# waits for CHAOS_SETTLE, which is that lease with the load balancer's own health check — thirty
# second interval, two failures — allowed for on top of it.
lease=10

work=$(mktemp -d)
checks=0
failures=0
experiment=
template=
probe=
cleanup_hook=

# ---------------------------------------------------------------------------- the stack

setup()
{
	local missing

	for missing in aws jq curl; do
		command -v "$missing" > /dev/null || die "$missing is not installed."
	done

	account=$(aws sts get-caller-identity --query Account --output text) \
		|| die "No AWS credentials."

	region=$(aws configure get region)
	region=${AWS_REGION:-${AWS_DEFAULT_REGION:-${region:-eu-west-2}}}

	url=${CHAOS_URL:-$(aws cloudformation describe-stacks --stack-name "$stack" \
		--query "Stacks[0].Outputs[?OutputKey=='Url'].OutputValue" --output text 2> /dev/null)}

	[ -n "${url:-}" ] && [ "$url" != None ] || die "The stack $stack has no Url output. Is it up?"

	base=$url/asyncdb

	role=${CHAOS_ROLE:-$(aws cloudformation describe-stacks --stack-name "$role_stack" \
		--query "Stacks[0].Outputs[?OutputKey=='ChaosRole'].OutputValue" --output text 2> /dev/null)}

	[ -n "${role:-}" ] && [ "$role" != None ] \
		|| die "No FIS role. Run 'make create-chaos-stack', or set CHAOS_ROLE."

	# The instances are found by tag inside this stack's own VPC rather than by tag alone, so a
	# second stack in the account is never a target. Nothing else is taken from the template.
	vpc=$(aws cloudformation describe-stack-resource --stack-name "$stack" \
		--logical-resource-id VPC --query 'StackResourceDetail.PhysicalResourceId' --output text)

	echo "Stack $stack at $url, vpc $vpc, role $role."
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

instance_arn()
{
	printf 'arn:aws:ec2:%s:%s:instance/%s' "$region" "$account" "$1"
}

subnet_arn()
{
	printf 'arn:aws:ec2:%s:%s:subnet/%s' "$region" "$account" "$1"
}

# A JSON array of arns, for pasting into an experiment template's resourceArns.
arns()
{
	local kind=$1 first=1 id
	shift

	printf '['
	for id in "$@"; do
		[ "$first" = 1 ] || printf ', '
		printf '"%s"' "$("${kind}_arn" "$id")"
		first=0
	done
	printf ']'
}

# Run Command is how a private instance is asked anything: the runner is outside the VPC and
# only the load balancer answers it, so a single node's own view of itself — which is the whole
# diagnostic in doc/runbook — is unreachable over HTTP. It is best effort by design: an instance
# that cannot be asked is reported and not asserted on.
ssm_run()
{
	local id=$1 command
	shift

	command=$(aws ssm send-command --instance-ids "$id" \
		--document-name AWS-RunShellScript \
		--parameters "commands=[$(jq -Rn --arg c "$*" '$c')]" \
		--query 'Command.CommandId' --output text 2> /dev/null) || return 1

	aws ssm wait command-executed --command-id "$command" --instance-id "$id" 2> /dev/null

	aws ssm get-command-invocation --command-id "$command" --instance-id "$id" \
		--query 'StandardOutputContent' --output text 2> /dev/null
}

# The base image is the ECS-optimised AMI, which runs an agent container of its own — so the
# asyncdb container is the one whose image says asyncdb, and never `docker ps -q | head -1`,
# which is how a rebuild that had plainly happened was read off the wrong container's logs.
container_logs()
{
	echo 'docker logs $(docker ps --format "{{.ID}} {{.Image}}" | awk "/asyncdb/{print \$1; exit}") 2>&1'
}

# What one node says about itself, rather than what the load balancer happened to route to.
node_health()
{
	ssm_run "$1" 'curl -s --max-time 5 http://localhost:8080/health'
}

# ---------------------------------------------------------------------------- the data

seed()
{
	local i status

	# The seed is written every run and not only the first. A key whose owner changed hands is
	# a key the new owner does not hold — there is no rebalancing, by design — so a run that
	# took the last run's seed on trust would start each experiment thinner than the one
	# before it. Writing it again is two hundred idempotent requests and makes runs repeatable.
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

	if [ "$missing" = 0 ]; then
		result 0 "$3"
	else
		result 1 "$3 — $missing of $1 records could not be read in $2 attempts: $(codes)"
	fi
}

codes()
{
	sort "$work/codes" | uniq -c | sort -rn | awk '{ printf "%s×%s ", $1, $2 }'
}

# expect_reads / expect_writes <how many> <description> — the check and the evidence together.
# A bare count is what made a failed run of this suite unreadable once.
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

scan_status()
{
	status "$base/table/$table/key?limit=100"
}

# ---------------------------------------------------------------------------- the probe

# Reads through the load balancer for as long as the fault lasts, so that an experiment can say
# what a client saw rather than only what the cluster looked like afterwards.
start_probe()
{
	rm -f "$work/probe"

	(
		while :; do
			curl --silent --output /dev/null --max-time 10 \
				--write-out '%{http_code}\n' \
				"$base/table/$table/key/$((RANDOM % records))"
		done > "$work/probe"
	) &

	probe=$!
}

stop_probe()
{
	[ -n "$probe" ] || return 0

	kill -- "-$probe" 2> /dev/null
	wait "$probe" 2> /dev/null
	probe=
}

# probe_report <description> — what the probe saw, reported and never asserted on. A read during
# the window in which the load balancer has not yet noticed a dead target is a read it sends to
# one, so a dip here is the load balancer's health check interval and not the database.
probe_report()
{
	local total answered

	total=$(wc -l < "$work/probe")
	answered=$(grep -c '^2' "$work/probe") || answered=0

	printf '  ---- %s: %s of %s reads answered 2xx\n' "$1" "$answered" "$total"
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

# Every sample has to match, and a sample that did not answer never does: the load balancer
# picks a different instance each time, so a predicate that holds for eight samples is one that
# holds for the cluster rather than for whichever node answered first.
health_matches()
{
	local i answer

	for (( i = 0; i < ${2:-8}; i++ )); do
		answer=$(curl --fail --silent --max-time 10 "$base/health") || return 1
		echo "$answer" | jq --exit-status "$1" > /dev/null 2>&1 || return 1
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
	last=$(curl --fail --silent --max-time 10 "$base/health")

	if [ -z "$last" ]; then
		result 1 "$3 — $base/health did not answer at all"
	else
		result 1 "$3 — health says $(echo "$last" \
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

# fis_start <template file> — create the experiment template, start it, and remember both so
# that the trap can stop and delete them whatever happens next.
fis_start()
{
	local answer

	# What the service said is the diagnosis and there is nowhere else to read it — an
	# experiment that never started leaves no experiment to ask about. Every one of these
	# failed identically once because this was thrown away.
	answer=$(aws fis create-experiment-template --cli-input-json "file://$1" \
		--query 'experimentTemplate.id' --output text 2>&1)

	if [ $? -ne 0 ]; then
		result 1 "the experiment template was not created"
		printf '  %s\n' "$answer"
		[ "${CHAOS_VALIDATE:-0}" = 1 ] && { verdict; exit 1; }
		return 1
	fi

	template=$answer

	# Validating is creating the template and deleting it again: the service checks every
	# action, parameter and target arn in it, and nothing is started, so it costs nothing and
	# touches nothing. chaos/validate.sh is this mode over every experiment.
	if [ "${CHAOS_VALIDATE:-0}" = 1 ]; then
		result 0 "the experiment template is accepted by the service"
		aws fis delete-experiment-template --id "$template" > /dev/null 2>&1
		template=
		verdict
		exit $?
	fi

	answer=$(aws fis start-experiment --experiment-template-id "$template" \
		--query 'experiment.id' --output text 2>&1)

	if [ $? -ne 0 ]; then
		result 1 "the experiment did not start"
		printf '  %s\n' "$answer"
		return 1
	fi

	experiment=$answer

	echo "  Experiment $experiment from template $template."
}

# Creating a template and deleting it again touches nothing and asks the service every question
# that matters: whether these credentials may drive FIS at all, and whether they may pass the
# role to it. Both are answered before an experiment has waited minutes to find out.
preflight_fis()
{
	local instance answer

	instance=$(instances asyncdb | cut -f1 | head -1)

	cat > "$work/preflight.json" <<EOF
{
	"description": "asyncdb chaos: preflight, never started",
	"roleArn": "$role",
	"stopConditions": [ { "source": "none" } ],
	"tags": { "Name": "asyncdb-chaos" },
	"targets": {
		"Nodes": {
			"resourceType": "aws:ec2:instance",
			"resourceArns": $(arns instance "$instance"),
			"selectionMode": "ALL"
		}
	},
	"actions": {
		"stop": {
			"actionId": "aws:ec2:stop-instances",
			"targets": { "Instances": "Nodes" }
		}
	}
}
EOF

	answer=$(aws fis create-experiment-template --cli-input-json "file://$work/preflight.json" \
		--query 'experimentTemplate.id' --output text 2>&1)

	if [ $? -ne 0 ]; then
		printf '%s\n' "$answer" | sed 's/^/  /' >&2

		# The two ways this fails are not the same thing, and only one of them is about
		# permissions. Saying so here saves reading an access denied that is not one.
		case $answer in
			*AccessDenied* | *not\ authorized*)
				echo >&2
				echo "The operator policy is part of chaos.json. If the chaos stack predates" >&2
				echo "it, 'make update-chaos-stack' attaches it, and it goes away with the" >&2
				echo "stack." >&2
				;;
			*)
				echo >&2
				echo "The credentials can reach the service, so this is the template and not" >&2
				echo "the permissions." >&2
				;;
		esac

		return 1
	fi

	aws fis delete-experiment-template --id "$answer" > /dev/null 2>&1
}

fis_status()
{
	aws fis get-experiment --id "$experiment" --query 'experiment.state.status' --output text
}

fis_reason()
{
	aws fis get-experiment --id "$experiment" --query 'experiment.state.reason' --output text
}

# An assertion made before the fault is applied is an assertion about nothing. FIS reports
# running once every action has started, and the fault itself lands a moment after that.
fis_await_running()
{
	local deadline=$((SECONDS + 300)) state

	while [ "$SECONDS" -lt "$deadline" ]; do
		state=$(fis_status)

		case $state in
			running | completed)
				result 0 "the fault was injected"
				sleep "$onset"
				return 0
				;;
			failed | stopped)
				result 1 "the fault was injected — $state: $(fis_reason)"
				return 1
				;;
		esac

		sleep 5
	done

	result 1 "the fault was injected — still $(fis_status) after five minutes"
	return 1
}

fis_await_end()
{
	local deadline=$((SECONDS + ${1:-900})) state

	while [ "$SECONDS" -lt "$deadline" ]; do
		state=$(fis_status)

		case $state in
			completed) echo "  The fault has been removed."; return 0 ;;
			stopped | failed) echo "  The experiment $state: $(fis_reason)"; return 1 ;;
		esac

		sleep 10
	done

	echo "  The experiment is still $(fis_status)."
	return 1
}

fis_finish()
{
	[ -n "$experiment" ] && aws fis stop-experiment --id "$experiment" > /dev/null 2>&1
	[ -n "$template" ] && aws fis delete-experiment-template --id "$template" > /dev/null 2>&1

	experiment=
	template=
}

# ---------------------------------------------------------------------------- the verdict

cleanup()
{
	local status=$?

	stop_probe
	fis_finish

	[ -n "$cleanup_hook" ] && "$cleanup_hook"

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
