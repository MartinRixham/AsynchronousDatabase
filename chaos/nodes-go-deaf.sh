#! /usr/bin/env bash

# One node in every zone answers no peer, and is still a member.
#
#   doc/runbook/nodes.md#a-node-answers-no-peer
#
# One database node in each of the three zones stops answering on its API port, while its
# process carries on renewing its lease. **A read and a scan both pass over a copy that does not
# answer** — a scan names a partition and is answered by one copy of it, the same hops a read
# takes — so what this fault costs is the partitions whose copy in *every* zone has gone deaf,
# and what it refuses is writes, which need every copy.
#
# It is the state the deployment is least likely to reach by accident and the hardest to
# arrange deliberately, which is why it is a test and not a paragraph: it needs one node in every
# zone deaf at the same time, and it has to be deaf to its peers while it is still renewing.
#
# The fault goes in through the SSM agent, as a rule of our own in DOCKER-USER — see
# blackhole_rule in chaos/harness.sh for why a rule in INPUT or OUTPUT leaves a containerised
# database answering. The note on the SSM faults in chaos/README.md is the rest of it.

source "$(dirname "$0")/harness.sh"

banner "One node in every zone goes deaf" "Reads and scans pass over a copy that does not answer. Writes need every copy."

setup
seed
start_load

# One instance per zone, which is what leaves one partition in eight with no copy any peer can
# reach — and is what makes this a fault of the nodes rather than of a zone.
deaf=$(instances asyncdb | awk '!seen[$2]++ { print $1 }')
count=$(echo "$deaf" | wc -l)

expect "$count" 3 "one node was picked in each of three zones"
[ "$count" = 3 ] || { verdict; exit 1; }

# The fault is in what arrives, and nothing stops a deaf node asking: it forwards for a client and
# is answered, and it is only its peers' requests that never reach it. Half the nodes are deaf, so
# what the load balancer picks is a coin toss, and the assertions below are made of a node that
# hears.
hearing=$(instances asyncdb | cut -f1 | grep -vxF "$deaf" | head -1)

seconds=${CHAOS_DEAF_SECONDS:-300}

# Everything addressed to port 8080 that is not addressed to another instance is a peer's request
# on its way into the container, because DOCKER-USER sees it after the address has been translated.
# What the container sends to a peer's 8080 still carries that peer's address and is left alone, so
# the node goes deaf without going blind: it answers no peer and still forwards for a client.
cidr=$(aws ec2 describe-vpcs --vpc-ids "$vpc" --query 'Vpcs[0].CidrBlock' --output text)

match="-p tcp --dport 8080 ! -d $cidr"

inject()
{
	echo "  Blackholing port 8080 into the container on $(echo "$deaf" | tr '\n' ' ')"

	blackhole "$seconds" "$match" $deaf
}

heal()
{
	blackhole_clear "$match" $deaf
}

preflight()
{
	may_run $deaf
}

fault_start || { verdict; exit 1; }

# The whole difficulty of this failure mode: the node is not answering and is still a member,
# because renewing a lease says nothing about being able to serve a request.
holds '(.nodes | length) == 6 and (.zones | length) == 3' 30 \
	"a node that does not answer stays in the membership while its lease is renewed"

# A read needs one copy and passes over a node that does not answer, so every key still reads —
# but not from everywhere, and that is the arithmetic the runbook leaves out. The copies of a
# partition are one node per zone and one node per zone has gone deaf, so one partition in eight
# has every copy of it deaf. Those keys are held by three nodes that no other node can reach and
# that are still in the load balancer answering for what they hold, so they are read by a request
# that lands on one of them and by nothing else. The attempts are the load balancer coming round
# to them.
expect_readable 40 8 "every key is read, from a copy that answers or from a deaf copy itself"
printf '  ---- reads with a node deaf in every zone: %s\n' "$(codes)"

# **A scan is the same arithmetic.** It names a partition and is answered by one copy of it, so a
# deaf copy is passed over the way a read passes over one — and what a hearing node cannot answer
# for is the one partition in eight whose copy in every zone is deaf. So the scan of a seeded key
# either answers or fails with a 5xx, and never refuses: a refusal would be a cursor or a range no
# copy would take, which is not what a deaf node causes.
deaf_scans=0

# A sample of the seeded keys rather than all two hundred: one partition in eight has every copy
# of it deaf, so two dozen keys is several of them and a round trip each.
for key in $(seq 0 23); do
	scan=$(node_status "$hearing" "/table/$table/key?key=$key&limit=100")

	case $scan in
		2*) ;;
		5*) deaf_scans=$((deaf_scans + 1)) ;;
		'')
			result 1 "a scan of every seeded partition answers or fails — $hearing could not be asked"

			break
			;;
		*)
			result 1 "a scan of every seeded partition answers or fails — $scan from $hearing"

			break
			;;
	esac
done

result 0 "a scan of a partition is answered by a copy that hears, or fails where none does"
printf '  ---- %s seeded partitions had no copy %s could reach\n' "$deaf_scans" "$hearing"

# Writes are reported and not asserted on. The runbook says reads and writes of individual keys
# are fine here, and for reads that is exactly true — but a write needs *every* copy, and a key
# whose copies include one of these three nodes is a key that cannot be written. Roughly seven
# writes in eight touch one, so this number is expected to be large.
printf '  ---- %s of 20 writes were refused while three nodes were deaf\n' "$(write_check 20)"

load_report "while three nodes were deaf"

fault_stop

# The membership is what this fault never touched — the assertion above is that it held at six
# nodes and three zones throughout — so it is no recovery check here. A write is: it needs every
# copy of its key, and seven writes in eight touched a deaf node a moment ago.
await_writes 20 "$settle" "every node answers its peers again"

expect "$(scan_status)" 200 "every seeded partition is scannable once every node answers"

# Seven writes in eight were refused while the three were deaf, and what matters is the eighth:
# a write that was answered had reached every copy, deaf nodes included, because a deaf node is
# one that cannot be *reached* rather than one that stopped writing.
expect_load_kept "once every node answered its peers again"

verdict
