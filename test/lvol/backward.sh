#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/lvol/common.sh

new_spdk=$rootdir/app/spdk_tgt/spdk_tgt
old_spdk=$1

function check_fields() {
	lvols=$(rpc_cmd bdev_get_bdevs | jq -r '[ .[] | select(.product_name == "Logical Volume") ]')
	[ "$(jq length <<< "$lvols")" == "2" ]
	[ "$lvols" = "$old_lvols" ]

	lvs=$(rpc_cmd bdev_lvol_get_lvstores)
	[ "$(jq length <<< "$lvs")" == "1" ]
	[ "$lvs" = "$old_lvs" ]
}

# create lvs + multiple lvols, verify their params
function lvol_backwards_compatibility() {
	# create an lvol store
	aio_bs=4096
	aio_bdev=$(rpc_cmd bdev_aio_create $testdir/aio_bdev aio_bdev $aio_bs)

	# round down lvol size to the nearest cluster size boundary
	# thick provision size is 1/2 aio bdev size, and thin provision size is times 1000 over aio size
	lvol_size_mb=$(( LVS_DEFAULT_CAPACITY_MB / 2 ))
	lvol_size_mb=$(( lvol_size_mb / LVS_DEFAULT_CLUSTER_SIZE_MB * LVS_DEFAULT_CLUSTER_SIZE_MB ))
	lvol_thin_size_mb=$(( lvol_size_mb * 2000 ))

	# create lvol store, thick and thin provisioned lvols
	lvs_uuid=$(rpc_cmd bdev_lvol_create_lvstore "$aio_bdev" lvs_test)
	lvol_uuid1=$(rpc_cmd bdev_lvol_create -u "$lvs_uuid" "lvol_test_thick" "$lvol_size_mb")
	lvol_uuid2=$(rpc_cmd bdev_lvol_create -t -u "$lvs_uuid" "lvol_test_thin" "$lvol_thin_size_mb")

	# make sure test is ran against SPDK v19.10.1
	version=$(rpc_cmd spdk_get_version)
	[ "$(jq -r '.version' <<< "$version")" = "SPDK v19.10.1" ]

	# save original lvs and lvol fields
	old_lvs=$(rpc_cmd bdev_lvol_get_lvstores)
	[ "$(jq length <<< "$old_lvs")" == "1" ]
	old_lvols=$(rpc_cmd bdev_get_bdevs | jq -r '[ .[] | select(.product_name == "Logical Volume") ]')
	[ "$(jq length <<< "$old_lvols")" == "2" ]

	# kill SPDK v19.10.1 and start current commit
	killprocess $spdk_pid
	$new_spdk &
	spdk_pid=$!
	waitforlisten $spdk_pid

	# make sure test is ran against pre-release SPDK 20.01
	version=$(rpc_cmd spdk_get_version)
	[ "$(jq -r '.fields.major' <<< "$version")" = "20" ]
	[ "$(jq -r '.fields.minor' <<< "$version")" = "1" ]
	[ "$(jq -r '.fields.patch' <<< "$version")" = "0" ]
	[ "$(jq -r '.fields.suffix' <<< "$version")" = "-pre" ]

	# reload lvol store
	aio_bdev=$(rpc_cmd bdev_aio_create $testdir/aio_bdev aio_bdev $aio_bs)

	# compare lvs and lvol fields after reloading in new SPDK version
	check_fields

	# kill pre-release SPDK 20.01 and start SPDK v19.10.1
	killprocess $spdk_pid
	$old_spdk &
	spdk_pid=$!
	waitforlisten $spdk_pid

	# make sure test is ran against SPDK v19.10.1
	version=$(rpc_cmd spdk_get_version)
	[ "$(jq -r '.version' <<< "$version")" = "SPDK v19.10.1" ]

	# reload lvol store
	aio_bdev=$(rpc_cmd bdev_aio_create $testdir/aio_bdev aio_bdev $aio_bs)

	# compare lvs and lvol fields after reloading in old SPDK version
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

run_test "lvol_backwards_compatibility" lvol_backwards_compatibility

trap - SIGINT SIGTERM EXIT
killprocess $spdk_pid
rm -f $testdir/aio_bdev
