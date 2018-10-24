#!/usr/bin/env bash

# This script should not contain any distro specfic part.
# All packages must be installed before.

ROOT_DIR=$(readlink -f $(dirname $0)/../../)

set -xe

which spdk_tgt

NRHUGE=512 ${ROOT_DIR}/scripts/setup.sh

rm -f /var/tmp/spdk.sock
spdk_tgt  &
trap "kill -9 $!" ERR

for i in $(seq 20); do
	echo -n .
	sleep 0.5
	[[ -S /var/tmp/spdk.sock ]] && break
done;

[[ ! -S /var/tmp/spdk.sock ]] && exit 1

spdk-rpc construct_malloc_bdev -b Malloc0 32 512 | grep Malloc0
spdk-rpc kill_instance SIGINT

for i in $(seq 20); do
	echo -n .
	sleep 0.5
	[[ ! -S /var/tmp/spdk.sock ]] && exit 0
done

exit 1
