#! /bin/bash

set -euo pipefail

dnf -y update

dnf -y install unzip

curl -fsS "https://awscli.amazonaws.com/awscli-exe-linux-x86_64.zip" -o /tmp/awscliv2.zip
unzip -q -d /tmp /tmp/awscliv2.zip
/tmp/aws/install --update
rm -rf /tmp/awscliv2.zip /tmp/aws

aws --version | grep -q '^aws-cli/2\.'

systemctl enable --now docker
docker info > /dev/null

mkdir -p /var/lib/asyncdb
