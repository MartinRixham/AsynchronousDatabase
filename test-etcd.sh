#!/usr/bin/env bash
# test-etcd-cluster.sh
# Usage: ./test-etcd-cluster.sh <LOAD_BALANCER_DNS_NAME>

LB_DNS=$1

if [ -z "$LB_DNS" ]; then
  echo "Usage: $0 <LOAD_BALANCER_DNS_NAME>"
  exit 1
fi

echo "Checking etcd cluster via Load Balancer: $LB_DNS"

# 1. Check health
echo "==> Checking cluster health..."
curl -s http://$LB_DNS:2379/health | jq .

# 2. Write a test key
echo "==> Writing test key..."
curl -s http://$LB_DNS:2379/v2/keys/testkey -XPUT -d value="hello-etcd" | jq .

# 3. Read the test key
echo "==> Reading test key..."
curl -s http://$LB_DNS:2379/v2/keys/testkey | jq .

# 4. Delete the test key
echo "==> Deleting test key..."
curl -s http://$LB_DNS:2379/v2/keys/testkey -XDELETE | jq .

echo "Done."

