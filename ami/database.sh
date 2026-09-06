#! /bin/bash

set -euo pipefail

dnf -y update

systemctl enable --now docker
docker info > /dev/null

mkdir -p /var/lib/asyncdb
