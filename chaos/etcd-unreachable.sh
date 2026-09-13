#! /usr/bin/env bash

# One node loses etcd and carries on with the membership it last read.
#
#   doc/runbook/membership.md#etcd-cannot-be-reached
#
# A single database node has its path to etcd blackholed, and nothing else about it changes. It
# can read no membership, so it keeps the one it had: its nodes are where they were, so a read it
# is given still goes to the copy that has the key. What it gives up is ordering writes, because
# the leases that membership was read under may have run out, so it refuses every write rather
# than taking one no leader ordered, and a lease later it takes itself out of the load balancer.
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

banner "One node loses etcd" "It keeps the membership it last read, and re-registers by itself when etcd comes back."

setup
seed
start_load

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
await_node "$isolated" '.etcd.registered == false and (.nodes | length) == 6' "$settle" \
	"the isolated node lost its registration and kept the membership it last read"

# And a node that can order no write says so a lease later, on the only channel the load balancer
# reads. It goes on serving reads and answering its peers, neither of which needs a leader.
await_node "$isolated" '.unled' "$settle" \
	"the isolated node reports itself unled, which is what takes it out of the load balancer"

# From the other five it is a node that stopped renewing, so it is gone within a lease. A sample
# that hits the isolated node itself still names the six it last read, and says it holds no
# registration.
await '((.nodes | length) == 5) or (.etcd.registered == false)' "$settle" \
	"the other nodes dropped it from the membership when its lease ran out"

# Every seeded record is written once, so a copy the isolated node holds is as good as any, and a
# key it holds no copy of is asked of a node that has one. A read the load balancer sends it while
# its health check is still failing is answered like any other.
expect_reads 60 "every read of a seeded record is answered while a node has lost etcd"

load_report "while one node had lost etcd"

fault_stop

# It re-registers from scratch on the pass after a renewal fails rather than believing it is
# still a member, which is why nothing has to be restarted.
await '(.nodes | length) == 6 and (.zones | length) == 3' "$settle" \
	"the isolated node re-registered by itself, with nothing restarted"

expect_reads 60 "every read of a seeded record is answered again"

# The isolated node went on serving what it held and refusing what no leader had ordered, so a
# write it was part of was either taken by every copy or refused outright.
expect_load_kept "once the isolated node had re-registered"

verdict
