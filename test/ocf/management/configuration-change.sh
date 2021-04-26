#!/usr/bin/env bash

curdir=$(dirname $(readlink -f "${BASH_SOURCE[0]}"))
rootdir=$(readlink -f $curdir/../../..)
source $rootdir/test/common/autotest_common.sh

rpc_py=$rootdir/scripts/rpc.py
cache_line_sizes=(4 8 16 32 64)
cache_modes=(wt wb pt wa wi wo)

$SPDK_BIN_DIR/iscsi_tgt &
spdk_pid=$!

waitforlisten $spdk_pid

# Create OCF cache with different cache line sizes
for cache_line_size in "${cache_line_sizes[@]}"; do
	$rpc_py bdev_malloc_create 101 512 -b Malloc0
	$rpc_py bdev_malloc_create 101 512 -b Malloc1
	$rpc_py bdev_ocf_create Cache0 wt Malloc0 Malloc1 --cache-line-size $cache_line_size

	$rpc_py bdev_ocf_get_bdevs | jq -e \
		'.[0] | .started and .cache.attached and .core.attached'

	# Check if cache line size values are reported correctly
	$rpc_py bdev_get_bdevs -b Cache0 | jq -e \
		".[0] | .driver_specific.cache_line_size == $cache_line_size"
	$rpc_py save_subsystem_config -n bdev | jq -e \
		".config | .[] | select(.method == \"bdev_ocf_create\") | .params.cache_line_size == $cache_line_size"

	$rpc_py bdev_ocf_delete Cache0
	$rpc_py bdev_malloc_delete Malloc0
	$rpc_py bdev_malloc_delete Malloc1
done

# Prepare OCF cache for dynamic configuration switching
$rpc_py bdev_malloc_create 101 512 -b Malloc0
$rpc_py bdev_malloc_create 101 512 -b Malloc1
$rpc_py bdev_ocf_create Cache0 wt Malloc0 Malloc1

$rpc_py bdev_ocf_get_bdevs | jq -e \
	'.[0] | .started and .cache.attached and .core.attached'

# Change cache mode
for cache_mode in "${cache_modes[@]}"; do
	$rpc_py bdev_ocf_set_cache_mode Cache0 $cache_mode

	# Check if cache mode values are reported correctly
	$rpc_py bdev_get_bdevs -b Cache0 | jq -e \
		".[0] | .driver_specific.mode == \"$cache_mode\""
	$rpc_py save_subsystem_config -n bdev | jq -e \
		".config | .[] | select(.method == \"bdev_ocf_create\") | .params.mode == \"$cache_mode\""
done

trap - SIGINT SIGTERM EXIT
killprocess $spdk_pid
