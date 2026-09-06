#! /bin/bash

set -euo pipefail

systemctl stop ecs || true
rm -rf /var/lib/ecs/data

cloud-init clean --logs

rm -f /home/ec2-user/.ssh/authorized_keys /root/.ssh/authorized_keys
