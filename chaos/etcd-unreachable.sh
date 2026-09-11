#! /usr/bin/env bash

# One node loses etcd and carries on as a cluster of one.
#
#   doc/runbook/membership.md#etcd-cannot-be-reached
#
# A single database node has its path to etcd blackholed, and nothing else about it changes. It
# reads no membership, so it puts itself in the list; a membership of one means it holds every
# key, answers every read out of its own store — 404 included, for keys it has never seen — and
# leads nothing, so it refuses every write rather than taking one no leader ordered.
#
# The runbook calls that the safe way to be wrong, and then says what is left of it: nothing here
# takes the node out of the load balancer, so it goes on being routed to and goes on answering
# for keys it has never held. This experiment is what shows both halves, which is why it is worth
# running even though nothing about it fails.
#
# It differs from etcd-quorum-lost.sh in the direction the fault points. There, etcd is broken
# for everyone; here, one node is broken for etcd, and the rest of the cluster carries on
# without it — which is what makes the recovery a re-registration rather than a repopulation.
#
# The fault goes in through the SSM agent, as a rule of our own in DOCKER-USER — see
# blackhole_rule in chaos/harness.sh for why a rule in INPUT or OUTPUT leaves a containerised
# database talking to etcd throughout. The note on the SSM faults in chaos/README.md is the rest
# of it.

source "$(dirname "$0")/harness.sh"

banner "One node loses etcd" "It becomes a cluster of one, and re-registers by itself when etcd comes back."

setup
seed

isolated=$(instances asyncdb | cut -f1 | head -1)

seconds=${CHAOS_ETCD_SECONDS:-240}

# Nothing else on a database node is forwarded to port 2379, so the port alone is the etcd client
# and no address is needed to name it. The node is left whole in every other direction: it serves
# every request it is given and answers every peer, which is what makes this one node losing etcd
# rather than one node losing the network.
match="-p tcp --dport 2379"

inject()
{
	echo "  Blackholing the path from $isolated to etcd."

	blackhole "$seconds" "$match" "$isolated"
}

heal()
{
	blackhole_clear "$match" "$isolated"
}

preflight()
{
	may_run "$isolated"
}

fault_start || { verdict; exit 1; }

# What the isolated node says about itself is the diagnosis, and only the node can be asked:
# the load balancer picks whichever instance it likes, and five of the six are fine. It is waited
# for rather than asked once, because what it is waiting on is a renewal that has to fail before
# there is anything to see, and the lease is ten seconds.
await_node "$isolated" '(.nodes | length) == 1' "$settle" \
	"the isolated node reports a membership of one, which is how this is recognised"

# And a membership of one is a node that can order no write, so a lease later it says so on the
# only channel the load balancer reads. It goes on serving the keys it holds and answering its
# peers, neither of which arrives this way.
await_node "$isolated" '.unled' "$settle" \
	"the isolated node reports itself unled, which is what takes it out of the load balancer"

# From the other five it is a node that stopped renewing, so it is gone within a lease. A sample
# that hits the isolated node itself is the one that has no nodes field, and both answers are
# the state this is looking for.
await '((.nodes | length) == 5) or ((.nodes | length) == 1)' "$settle" \
	"the other nodes dropped it from the membership when its lease ran out"

# The half of the runbook page that is a warning rather than a description, and the window it
# lives in. The node takes itself out of service, but only the health check carries that and the
# load balancer routes to it until the check has failed twice — and what it answers meanwhile for
# a key it has never held is a 404 rather than a question asked of the copies that have it. How
# many of these reads fall inside that window is the check's interval and not a claim about the
# database, so it is counted and never asserted on.
missed=$(read_check 60)
printf '  ---- %s of 60 reads answered something other than 2xx\n' "$missed"

[ "$missed" -gt 0 ] \
	&& echo "  ---- a node that has lost etcd and is still being chosen, exactly as documented"

fault_stop

# It re-registers from scratch on the pass after a renewal fails rather than believing it is
# still a member, which is why nothing has to be restarted.
await '(.nodes | length) == 6 and (.zones | length) == 3' "$settle" \
	"the isolated node re-registered by itself, with nothing restarted"

expect_reads 60 "every read of a seeded record is answered again"

verdict
