#! /bin/bash

set -e

docker-compose build

(cd ui
npm install
npm test
npm run build)

docker-compose up
