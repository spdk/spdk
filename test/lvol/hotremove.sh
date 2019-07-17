#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/lvol/common.sh

# create an lvol on lvs, then remove the lvs
function test_hotremove_lvol_store() {
	# create lvs + lvol on top
	malloc_name=$(rpc_cmd construct_malloc_bdev $MALLOC_SIZE_MB $MALLOC_BS)
	lvs_uuid=$(rpc_cmd construct_lvol_store "$malloc_name" lvs_test)

	# remove lvs (with one lvol open)
	rpc_cmd destroy_lvol_store -u "$lvs_uuid"
	! rpc_cmd get_lvol_stores -u "$lvs_uuid"
	lvolstores=$(rpc_cmd get_lvol_stores)
	[ "$(jq length <<< "$lvolstores")" == "0" ]

	# make sure the lvol is gone as well
	! rpc_cmd get_bdevs -b "$lvol_uuid"
	lvols=$(rpc_cmd get_bdevs | jq -r '[ .[] | select(.product_name == "Logical Volume") ]')
	[ "$(jq length <<< "$lvols")" == "0" ]

	# clean up
	rpc_cmd delete_malloc_bdev "$malloc_name"
}

# create an lvol on lvs, then remove the lvs (using only its alias)
function test_hotremove_lvol_store_by_alias() {
	# create lvs + lvol on top
	malloc_name=$(rpc_cmd construct_malloc_bdev $MALLOC_SIZE_MB $MALLOC_BS)
	lvs_uuid=$(rpc_cmd construct_lvol_store "$malloc_name" lvs_test)

	# remove lvs (with one lvol open)
	rpc_cmd destroy_lvol_store -l lvs_test
	! rpc_cmd get_lvol_stores -u "$lvs_uuid"
	lvolstores=$(rpc_cmd get_lvol_stores)
	[ "$(jq length <<< "$lvolstores")" == "0" ]

	# make sure the lvol is gone as well
	! rpc_cmd get_bdevs -b "$lvol_uuid"
	lvols=$(rpc_cmd get_bdevs | jq -r '[ .[] | select(.product_name == "Logical Volume") ]')
	[ "$(jq length <<< "$lvols")" == "0" ]

	# clean up
	rpc_cmd delete_malloc_bdev "$malloc_name"
}

# create an lvs on malloc, then remove just the malloc
function test_hotremove_lvol_store_base() {
	# create lvs + lvol on top
	malloc_name=$(rpc_cmd construct_malloc_bdev $MALLOC_SIZE_MB $MALLOC_BS)
	lvs_uuid=$(rpc_cmd construct_lvol_store "$malloc_name" lvs_test)

	# clean up
	rpc_cmd delete_malloc_bdev "$malloc_name"
	# make sure the lvs is gone
	! rpc_cmd get_lvol_stores -u "$lvs_uuid"
}

# create an lvs on malloc, then an lvol, then remove just the malloc
function test_hotremove_lvol_base() {
	# create lvs + lvol on top
	malloc_name=$(rpc_cmd construct_malloc_bdev $MALLOC_SIZE_MB $MALLOC_BS)
	lvs_uuid=$(rpc_cmd construct_lvol_store "$malloc_name" lvs_test)
	lvol_uuid=$(rpc_cmd construct_lvol_bdev -u "$lvs_uuid" lvol_test "$LVS_DEFAULT_CAPACITY_MB")

	rpc_cmd get_bdevs -b "$lvol_uuid"

	# clean up
	rpc_cmd delete_malloc_bdev "$malloc_name"
	# make sure the lvol is gone
	! rpc_cmd get_bdevs -b "$lvol_uuid"
	# make sure the lvs is gone as well
	! rpc_cmd get_lvol_stores -u "$lvs_uuid"
}

$rootdir/app/spdk_tgt/spdk_tgt &
spdk_pid=$!
trap "killprocess $spdk_pid; exit 1" SIGINT SIGTERM EXIT
waitforlisten $spdk_pid

run_test test_hotremove_lvol_store
run_test test_hotremove_lvol_store_by_alias
run_test test_hotremove_lvol_store_base
run_test test_hotremove_lvol_base

trap - SIGINT SIGTERM EXIT
killprocess $spdk_pid
