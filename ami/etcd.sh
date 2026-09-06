#! /bin/bash

set -euo pipefail

ETCD_IMAGE=quay.io/coreos/etcd:v3.5.9

dnf -y update

systemctl enable --now docker
docker info > /dev/null

docker pull "$ETCD_IMAGE"

mkdir -p /var/lib/etcd
