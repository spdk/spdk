#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh

MALLOC_SIZE_MB=128
MALLOC_BS=512
LVS_DEFAULT_CLUSTER_SIZE_MB=4
LVS_DEFAULT_CLUSTER_SIZE=$(( LVS_DEFAULT_CLUSTER_SIZE_MB * 1024 * 1024 ))
# reserve some MBs for lvolstore metadata
LVS_DEFAULT_CAPACITY_MB=$(( MALLOC_SIZE_MB - LVS_DEFAULT_CLUSTER_SIZE_MB ))
LVS_DEFAULT_CAPACITY=$(( LVS_DEFAULT_CAPACITY_MB * 1024 * 1024 ))

# create empty lvol store and verify its parameters
function test_construct_lvs_basic() {
	# create an lvol store
	malloc_name=$(rpc_cmd construct_malloc_bdev $MALLOC_SIZE_MB $MALLOC_BS)
	lvs_uuid=$(rpc_cmd construct_lvol_store "$malloc_name" lvs_test)
	lvs=$(rpc_cmd get_lvol_stores -u "$lvs_uuid")

	# verify it's there
	[ "$(jq -r '.[0].uuid' <<< "$lvs")" = "$lvs_uuid" ]
	[ "$(jq -r '.[0].name' <<< "$lvs")" = "lvs_test" ]
	[ "$(jq -r '.[0].base_bdev' <<< "$lvs")" = "$malloc_name" ]

	# verify some of its parameters
	cluster_size=$(jq -r '.[0].cluster_size' <<< "$lvs")
	[ "$cluster_size" = "$LVS_DEFAULT_CLUSTER_SIZE" ]
	total_clusters=$(jq -r '.[0].total_data_clusters' <<< "$lvs")
	[ "$(jq -r '.[0].free_clusters' <<< "$lvs")" = "$total_clusters" ]
	[ "$(( total_clusters * cluster_size ))" = "$LVS_DEFAULT_CAPACITY" ]

	# remove it and verify it's gone
	rpc_cmd destroy_lvol_store -u "$lvs_uuid"
	! rpc_cmd get_lvol_stores -u "$lvs_uuid"
	rpc_cmd delete_malloc_bdev "$malloc_name"
}

# create lvs + lvol on top, verify lvol's parameters
function test_construct_lvol_basic() {
	# create an lvol store
	malloc_name=$(rpc_cmd construct_malloc_bdev $MALLOC_SIZE_MB $MALLOC_BS)
	lvs_uuid=$(rpc_cmd construct_lvol_store "$malloc_name" lvs_test)

	# create an lvol on top
	lvol_uuid=$(rpc_cmd construct_lvol_bdev -u "$lvs_uuid" lvol_test "$LVS_DEFAULT_CAPACITY_MB")
	lvol=$(rpc_cmd get_bdevs -b "$lvol_uuid")

	[ "$(jq -r '.[0].name' <<< "$lvol")" = "$lvol_uuid" ]
	[ "$(jq -r '.[0].uuid' <<< "$lvol")" = "$lvol_uuid" ]
	[ "$(jq -r '.[0].aliases[0]' <<< "$lvol")" = "lvs_test/lvol_test" ]
	[ "$(jq -r '.[0].block_size' <<< "$lvol")" = "$MALLOC_BS" ]
	[ "$(jq -r '.[0].num_blocks' <<< "$lvol")" = "$(( LVS_DEFAULT_CAPACITY / MALLOC_BS ))" ]
	[ "$(jq -r '.[0].driver_specific.lvol.lvol_store_uuid' <<< "$lvol")" = "$lvs_uuid" ]

	# clean up and create another lvol, this time use lvs alias instead of uuid
	rpc_cmd destroy_lvol_bdev "$lvol_uuid"
	! rpc_cmd get_bdevs -b "$lvol_uuid"
	lvol_uuid=$(rpc_cmd construct_lvol_bdev -l lvs_test lvol_test "$LVS_DEFAULT_CAPACITY_MB")
	lvol=$(rpc_cmd get_bdevs -b "$lvol_uuid")

	[ "$(jq -r '.[0].name' <<< "$lvol")" = "$lvol_uuid" ]
	[ "$(jq -r '.[0].uuid' <<< "$lvol")" = "$lvol_uuid" ]
	[ "$(jq -r '.[0].aliases[0]' <<< "$lvol")" = "lvs_test/lvol_test" ]
	[ "$(jq -r '.[0].block_size' <<< "$lvol")" = "$MALLOC_BS" ]
	[ "$(jq -r '.[0].num_blocks' <<< "$lvol")" = "$(( LVS_DEFAULT_CAPACITY / MALLOC_BS ))" ]
	[ "$(jq -r '.[0].driver_specific.lvol.lvol_store_uuid' <<< "$lvol")" = "$lvs_uuid" ]

	# clean up
	rpc_cmd destroy_lvol_bdev "$lvol_uuid"
	! rpc_cmd get_bdevs -b "$lvol_uuid"
	rpc_cmd destroy_lvol_store -u "$lvs_uuid"
	! rpc_cmd get_lvol_stores -u "$lvs_uuid"
	rpc_cmd delete_malloc_bdev "$malloc_name"
}

# create lvs + multiple lvols, verify their params
function test_construct_multi_lvols_basic() {
	# create an lvol store
	malloc_name=$(rpc_cmd construct_malloc_bdev $MALLOC_SIZE_MB $MALLOC_BS)
	lvs_uuid=$(rpc_cmd construct_lvol_store "$malloc_name" lvs_test)

	# create 4 lvols
	lvol_size_mb=$(( LVS_DEFAULT_CAPACITY_MB / 4 ))
	# round down lvol size to the nearest cluster size boundary
	lvol_size_mb=$(( lvol_size_mb / LVS_DEFAULT_CLUSTER_SIZE_MB * LVS_DEFAULT_CLUSTER_SIZE_MB ))
	lvol_size=$(( lvol_size_mb * 1024 * 1024 ))
	for i in $(seq 1 4); do
		lvol_uuid=$(rpc_cmd construct_lvol_bdev -u "$lvs_uuid" "lvol_test${i}" "$lvol_size_mb")
		lvol=$(rpc_cmd get_bdevs -b "$lvol_uuid")

		[ "$(jq -r '.[0].name' <<< "$lvol")" = "$lvol_uuid" ]
		[ "$(jq -r '.[0].uuid' <<< "$lvol")" = "$lvol_uuid" ]
		[ "$(jq -r '.[0].aliases[0]' <<< "$lvol")" = "lvs_test/lvol_test${i}" ]
		[ "$(jq -r '.[0].block_size' <<< "$lvol")" = "$MALLOC_BS" ]
		[ "$(jq -r '.[0].num_blocks' <<< "$lvol")" = "$(( lvol_size / MALLOC_BS ))" ]
	done

	lvols=$(rpc_cmd get_bdevs | jq -r '[ .[] | select(.product_name == "Logical Volume") ]')
	[ "$(jq length <<< "$lvols")" == "4" ]

	# remove all lvols
	for i in $(seq 0 3); do
		lvol_uuid=$(jq -r ".[$i].name" <<< "$lvols")
		rpc_cmd destroy_lvol_bdev "$lvol_uuid"
	done
	lvols=$(rpc_cmd get_bdevs | jq -r '[ .[] | select(.product_name == "Logical Volume") ]')
	[ "$(jq length <<< "$lvols")" == "0" ]

	# create the same 4 lvols again and perform the same checks
	for i in $(seq 1 4); do
		lvol_uuid=$(rpc_cmd construct_lvol_bdev -u "$lvs_uuid" "lvol_test${i}" "$lvol_size_mb")
		lvol=$(rpc_cmd get_bdevs -b "$lvol_uuid")

		[ "$(jq -r '.[0].name' <<< "$lvol")" = "$lvol_uuid" ]
		[ "$(jq -r '.[0].uuid' <<< "$lvol")" = "$lvol_uuid" ]
		[ "$(jq -r '.[0].aliases[0]' <<< "$lvol")" = "lvs_test/lvol_test${i}" ]
		[ "$(jq -r '.[0].block_size' <<< "$lvol")" = "$MALLOC_BS" ]
		[ "$(jq -r '.[0].num_blocks' <<< "$lvol")" = "$(( lvol_size / MALLOC_BS ))" ]
	done

	lvols=$(rpc_cmd get_bdevs | jq -r '[ .[] | select(.product_name == "Logical Volume") ]')
	[ "$(jq length <<< "$lvols")" == "4" ]

	# clean up
	for i in $(seq 0 3); do
		lvol_uuid=$(jq -r ".[$i].name" <<< "$lvols")
		rpc_cmd destroy_lvol_bdev "$lvol_uuid"
	done
	lvols=$(rpc_cmd get_bdevs | jq -r '[ .[] | select(.product_name == "Logical Volume") ]')
	[ "$(jq length <<< "$lvols")" == "0" ]

	rpc_cmd destroy_lvol_store -u "$lvs_uuid"
	! rpc_cmd get_lvol_stores -u "$lvs_uuid"
	rpc_cmd delete_malloc_bdev "$malloc_name"
}

# create 2 lvolstores, each with a single lvol on top.
# use a single alias for both lvols, there should be no conflict
# since they're in different lvolstores
function test_construct_lvols_conflict_alias() {
	# create an lvol store 1
	malloc1_name=$(rpc_cmd construct_malloc_bdev $MALLOC_SIZE_MB $MALLOC_BS)
	lvs1_uuid=$(rpc_cmd construct_lvol_store "$malloc1_name" lvs_test1)

	# create an lvol on lvs1
	lvol1_uuid=$(rpc_cmd construct_lvol_bdev -l lvs_test1 lvol_test "$LVS_DEFAULT_CAPACITY_MB")
	lvol1=$(rpc_cmd get_bdevs -b "$lvol1_uuid")

	# use a different size for second malloc to keep those differentiable
	malloc2_size_mb=$(( MALLOC_SIZE_MB / 2 ))

	# create an lvol store 2
	malloc2_name=$(rpc_cmd construct_malloc_bdev $malloc2_size_mb $MALLOC_BS)
	lvs2_uuid=$(rpc_cmd construct_lvol_store "$malloc2_name" lvs_test2)

	lvol2_size_mb=$(( MALLOC_SIZE_MB / 2 - LVS_DEFAULT_CLUSTER_SIZE_MB ))
	# round down lvol size to the nearest cluster size boundary
	lvol2_size_mb=$(( lvol2_size_mb / LVS_DEFAULT_CLUSTER_SIZE_MB * LVS_DEFAULT_CLUSTER_SIZE_MB ))

	# create an lvol on lvs2
	lvol2_uuid=$(rpc_cmd construct_lvol_bdev -l lvs_test2 lvol_test "$lvol2_size_mb")
	lvol2=$(rpc_cmd get_bdevs -b "$lvol2_uuid")

	[ "$(jq -r '.[0].name' <<< "$lvol1")" = "$lvol1_uuid" ]
	[ "$(jq -r '.[0].uuid' <<< "$lvol1")" = "$lvol1_uuid" ]
	[ "$(jq -r '.[0].aliases[0]' <<< "$lvol1")" = "lvs_test1/lvol_test" ]
	[ "$(jq -r '.[0].driver_specific.lvol.lvol_store_uuid' <<< "$lvol1")" = "$lvs1_uuid" ]

	[ "$(jq -r '.[0].name' <<< "$lvol2")" = "$lvol2_uuid" ]
	[ "$(jq -r '.[0].uuid' <<< "$lvol2")" = "$lvol2_uuid" ]
	[ "$(jq -r '.[0].aliases[0]' <<< "$lvol2")" = "lvs_test2/lvol_test" ]
	[ "$(jq -r '.[0].driver_specific.lvol.lvol_store_uuid' <<< "$lvol2")" = "$lvs2_uuid" ]

	# clean up
	rpc_cmd destroy_lvol_store -u "$lvs1_uuid"
	! rpc_cmd get_lvol_stores -u "$lvs1_uuid"
	rpc_cmd destroy_lvol_store -u "$lvs2_uuid"
	! rpc_cmd get_lvol_stores -u "$lvs2_uuid"
	rpc_cmd delete_malloc_bdev "$malloc1_name"
	! rpc_cmd get_bdevs -b "$malloc1_name"
	rpc_cmd delete_malloc_bdev "$malloc2_name"
}

# try to create an lvol on inexistent lvs uuid
function test_construct_lvol_inexistent_lvs() {
	# create an lvol store
	malloc_name=$(rpc_cmd construct_malloc_bdev $MALLOC_SIZE_MB $MALLOC_BS)
	lvs_uuid=$(rpc_cmd construct_lvol_store "$malloc_name" lvs_test)

	# try to create an lvol on inexistent lvs
	dummy_uuid="00000000-0000-0000-0000-000000000000"
	! rpc_cmd construct_lvol_bdev -u "$dummy_uuid" lvol_test "$LVS_DEFAULT_CAPACITY_MB"

	lvols=$(rpc_cmd get_bdevs | jq -r '[ .[] | select(.product_name == "Logical Volume") ]')
	[ "$(jq length <<< "$lvols")" == "0" ]

	# clean up
	rpc_cmd destroy_lvol_store -u "$lvs_uuid"
	! rpc_cmd get_lvol_stores -u "$lvs_uuid"
	rpc_cmd delete_malloc_bdev "$malloc_name"
}

# try to create lvol on full lvs
function test_construct_lvol_full_lvs() {
	# create an lvol store
	malloc_name=$(rpc_cmd construct_malloc_bdev $MALLOC_SIZE_MB $MALLOC_BS)
	lvs_uuid=$(rpc_cmd construct_lvol_store "$malloc_name" lvs_test)

	# create valid lvol
	lvol1_uuid=$(rpc_cmd construct_lvol_bdev -l lvs_test lvol_test1 "$LVS_DEFAULT_CAPACITY_MB")
	lvol1=$(rpc_cmd get_bdevs -b "$lvol1_uuid")

	# try to create an lvol on lvs without enough free clusters
	! rpc_cmd construct_lvol_bdev -l lvs_test lvol_test2 "$lvol_size_mb"

	# clean up
	rpc_cmd destroy_lvol_store -u "$lvs_uuid"
	! rpc_cmd get_lvol_stores -u "$lvs_uuid"
	rpc_cmd delete_malloc_bdev "$malloc_name"
}

# try to create two lvols with conflicting aliases
function test_construct_lvol_alias_conflict() {
	# create an lvol store
	malloc_name=$(rpc_cmd construct_malloc_bdev $MALLOC_SIZE_MB $MALLOC_BS)
	lvs_uuid=$(rpc_cmd construct_lvol_store "$malloc_name" lvs_test)

	lvol_size_mb=$(( LVS_DEFAULT_CAPACITY_MB / 2 ))
	# round down lvol size to the nearest cluster size boundary
	lvol_size_mb=$(( lvol_size_mb / LVS_DEFAULT_CLUSTER_SIZE_MB * LVS_DEFAULT_CLUSTER_SIZE_MB ))

	# create valid lvol
	lvol1_uuid=$(rpc_cmd construct_lvol_bdev -l lvs_test lvol_test "$lvol_size_mb")
	lvol1=$(rpc_cmd get_bdevs -b "$lvol1_uuid")

	# try to create another lvol with a name that's already taken
	! rpc_cmd construct_lvol_bdev -l lvs_test lvol_test "$lvol_size_mb"

	# clean up
	rpc_cmd destroy_lvol_store -u "$lvs_uuid"
	! rpc_cmd get_lvol_stores -u "$lvs_uuid"
	rpc_cmd delete_malloc_bdev "$malloc_name"
	! rpc_cmd get_bdevs -b "$malloc_name"
}

function run_test() {
	$@

	leftover_bdevs=$(rpc_cmd get_bdevs)
	[ "$(jq length <<< "$leftover_bdevs")" == "0" ]
	leftover_lvs=$(rpc_cmd get_lvol_stores)
	[ "$(jq length <<< "$leftover_lvs")" == "0" ]
}

$rootdir/app/spdk_tgt/spdk_tgt &
spdk_pid=$!
trap "killprocess $spdk_pid; exit 1" SIGINT SIGTERM EXIT
waitforlisten $spdk_pid

run_test test_construct_lvs_basic
run_test test_construct_lvol_basic
run_test test_construct_multi_lvols_basic
run_test test_construct_lvols_conflict_alias
run_test test_construct_lvol_inexistent_lvs
run_test test_construct_lvol_full_lvs
run_test test_construct_lvol_alias_conflict

trap - SIGINT SIGTERM EXIT
killprocess $spdk_pid
