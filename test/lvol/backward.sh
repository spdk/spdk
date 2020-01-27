#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/lvol/common.sh

new_spdk=$rootdir/app/spdk_tgt/spdk_tgt
old_spdk=$1

function check_fields() {
	lvol=$(rpc_cmd bdev_get_bdevs -b "$lvol_uuid1")

	[ "$(jq -r '.[0].name' <<< "$lvol")" = "$lvol_uuid1" ]
	[ "$(jq -r '.[0].uuid' <<< "$lvol")" = "$lvol_uuid1" ]
	[ "$(jq -r '.[0].aliases[0]' <<< "$lvol")" = "lvs_test/lvol_test_thick" ]
	[ "$(jq -r '.[0].block_size' <<< "$lvol")" = "$aio_bs" ]
	[ "$(jq -r '.[0].num_blocks' <<< "$lvol")" = "$(( lvol_size / aio_bs ))" ]
	[ "$(jq -r '.[].driver_specific.lvol.thin_provision' <<< "$lvol")" = "false" ]

	lvol=$(rpc_cmd bdev_get_bdevs -b "$lvol_uuid2")

	[ "$(jq -r '.[0].name' <<< "$lvol")" = "$lvol_uuid2" ]
	[ "$(jq -r '.[0].uuid' <<< "$lvol")" = "$lvol_uuid2" ]
	[ "$(jq -r '.[0].aliases[0]' <<< "$lvol")" = "lvs_test/lvol_test_thin" ]
	[ "$(jq -r '.[0].block_size' <<< "$lvol")" = "$aio_bs" ]
	[ "$(jq -r '.[0].num_blocks' <<< "$lvol")" = "$(( lvol_size / aio_bs ))" ]
	[ "$(jq -r '.[].driver_specific.lvol.thin_provision' <<< "$lvol")" = "true" ]

	lvols=$(rpc_cmd bdev_get_bdevs | jq -r '[ .[] | select(.product_name == "Logical Volume") ]')
	[ "$(jq length <<< "$lvols")" == "2" ]
}

# create lvs + multiple lvols, verify their params
function test_backwards_compatibility() {
	# create an lvol store
	aio_bs=4096
	aio_bdev=$(rpc_cmd bdev_aio_create $testdir/aio_bdev aio_bdev $aio_bs)
	lvs_uuid=$(rpc_cmd bdev_lvol_create_lvstore "$aio_bdev" lvs_test)

	# round down lvol size to the nearest cluster size boundary
	lvol_size_mb=$(( LVS_DEFAULT_CAPACITY_MB / 2 ))
	lvol_size_mb=$(( lvol_size_mb / LVS_DEFAULT_CLUSTER_SIZE_MB * LVS_DEFAULT_CLUSTER_SIZE_MB ))
	lvol_size=$(( lvol_size_mb * 1024 * 1024 ))

	# Create thick provisioned lvol
	lvol_uuid1=$(rpc_cmd bdev_lvol_create -u "$lvs_uuid" "lvol_test_thick" "$lvol_size_mb")
	# Create thin provisioned lvol
	lvol_uuid2=$(rpc_cmd bdev_lvol_create -t -u "$lvs_uuid" "lvol_test_thin" "$lvol_size_mb")

	version=$(rpc_cmd spdk_get_version)
	[ "$(jq -r '.version' <<< "$version")" = "SPDK v19.10.1" ]

	check_fields

	# Restart SPDK master app
	killprocess $spdk_pid
	$new_spdk &
	spdk_pid=$!
	waitforlisten $spdk_pid

	# Load lvol store again
	aio_bdev=$(rpc_cmd bdev_aio_create $testdir/aio_bdev aio_bdev $aio_bs)

	version=$(rpc_cmd spdk_get_version)
	[ "$(jq -r '.fields.major' <<< "$version")" = "20" ]
	[ "$(jq -r '.fields.minor' <<< "$version")" = "1" ]
	[ "$(jq -r '.fields.patch' <<< "$version")" = "0" ]
	[ "$(jq -r '.fields.suffix' <<< "$version")" = "-pre" ]
	check_fields

	# Restart SPDK 19.10.1 app
	killprocess $spdk_pid
	$old_spdk &
	spdk_pid=$!
	waitforlisten $spdk_pid

	# Load lvol store again
	aio_bdev=$(rpc_cmd bdev_aio_create $testdir/aio_bdev aio_bdev $aio_bs)

	version=$(rpc_cmd spdk_get_version)
	[ "$(jq -r '.version' <<< "$version")" = "SPDK v19.10.1" ]

	check_fields

	# clean up
	rpc_cmd bdev_lvol_delete_lvstore -u "$lvs_uuid"
	rpc_cmd bdev_lvol_get_lvstores -u "$lvs_uuid" && false
	rpc_cmd bdev_aio_delete "$aio_bdev"
	check_leftover_devices
}

$old_spdk &
spdk_pid=$!
trap 'killprocess "$spdk_pid"; rm -f $testdir/aio_bdev; exit 1' SIGINT SIGTERM EXIT
waitforlisten $spdk_pid
truncate -s 400M $testdir/aio_bdev

run_test "test_backwards_compatibility" test_backwards_compatibility

trap - SIGINT SIGTERM EXIT
killprocess $spdk_pid
rm -f $testdir/aio_bdev
