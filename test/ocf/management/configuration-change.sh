#!/usr/bin/env bash

curdir=$(dirname $(readlink -f "${BASH_SOURCE[0]}"))
rootdir=$(readlink -f $curdir/../../..)
source $rootdir/test/common/autotest_common.sh

rpc_py=$rootdir/scripts/rpc.py
cache_modes=(wt wb pt wa wi wo)

$SPDK_BIN_DIR/iscsi_tgt &
spdk_pid=$!

waitforlisten $spdk_pid

# Prepare OCF cache
$rpc_py bdev_malloc_create 101 512 -b Malloc0
$rpc_py bdev_malloc_create 101 512 -b Malloc1
$rpc_py bdev_ocf_create Cache wt Malloc0 Malloc1

$rpc_py bdev_ocf_get_bdevs | jq -e \
	'.[0] | .started and .cache.attached and .core.attached'

# Change cache mode
for cache_mode in "${cache_modes[@]}"; do
	$rpc_py bdev_ocf_set_cache_mode Cache $cache_mode
done

trap - SIGINT SIGTERM EXIT
killprocess $spdk_pid
