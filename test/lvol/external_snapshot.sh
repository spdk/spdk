#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) 2022-2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
#
testdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$testdir/../..")
source "$rootdir/test/common/autotest_common.sh"
source "$rootdir/test/lvol/common.sh"
source "$rootdir/test/bdev/nbd_common.sh"

set -u

g_nbd_dev=INVALID
g_cluster_size=INVALID
g_block_size=INVALID

function test_esnap_reload() {
	local bs_dev esnap_dev
	local block_size=512
	local esnap_size_mb=1
	local lvs_cluster_size=$((16 * 1024))
	local lvs_uuid esnap_uuid eclone_uuid snap_uuid clone_uuid uuid
	local aio_bdev=test_esnap_reload_aio0

	# Create the lvstore on an aio device. Can't use malloc because we need to remove
	# the device and re-add it to trigger an lvstore unload and then load.
	rm -f $testdir/aio_bdev_0
	truncate -s "${AIO_SIZE_MB}M" $testdir/aio_bdev_0
	bs_dev=$(rpc_cmd bdev_aio_create "$testdir/aio_bdev_0" "$aio_bdev" "$block_size")
	lvs_uuid=$(rpc_cmd bdev_lvol_create_lvstore -c "$lvs_cluster_size" "$bs_dev" lvs_test)

	# Create a bdev that will be the external snapshot
	esnap_uuid=e4b40d8b-f623-416d-8234-baf5a4c83cbd
	esnap_dev=$(rpc_cmd bdev_malloc_create -u "$esnap_uuid" "$esnap_size_mb" "$block_size")
	eclone_uuid=$(rpc_cmd bdev_lvol_clone_bdev "$esnap_uuid" lvs_test "eclone1")

	# Unload the lvstore
	rpc_cmd bdev_aio_delete "$aio_bdev"
	NOT rpc_cmd bdev_lvol_get_lvstores -l lvs_test

	# Load the lvstore, expect to see eclone1 again
	bs_dev=$(rpc_cmd bdev_aio_create "$testdir/aio_bdev_0" "$aio_bdev" "$block_size")
	lvs_uuid=$(rpc_cmd bdev_lvol_get_lvstores -l lvs_test)
	uuid=$(rpc_cmd bdev_get_bdevs -b lvs_test/eclone1 | jq -r '.[].name')
	[[ "$uuid" == "$eclone_uuid" ]]

	# Create a snapshot of the eclone, reload, and verify all is there.
	snap_uuid=$(rpc_cmd bdev_lvol_snapshot "$eclone_uuid" snap1)
	rpc_cmd bdev_aio_delete "$aio_bdev"
	NOT rpc_cmd bdev_lvol_get_lvstores -l lvs_test
	bs_dev=$(rpc_cmd bdev_aio_create "$testdir/aio_bdev_0" "$aio_bdev" "$block_size")
	lvs_uuid=$(rpc_cmd bdev_lvol_get_lvstores -l lvs_test)
	uuid=$(rpc_cmd bdev_get_bdevs -b lvs_test/eclone1 | jq -r '.[].name')
	[[ "$uuid" == "$eclone_uuid" ]]
	uuid=$(rpc_cmd bdev_get_bdevs -b lvs_test/snap1 | jq -r '.[].name')
	[[ "$uuid" == "$snap_uuid" ]]

	# Create a clone of the snapshot, reload, and verify all is there.
	clone_uuid=$(rpc_cmd bdev_lvol_clone "$snap_uuid" clone1)
	rpc_cmd bdev_aio_delete "$aio_bdev"
	NOT rpc_cmd bdev_lvol_get_lvstores -l lvs_test
	bs_dev=$(rpc_cmd bdev_aio_create "$testdir/aio_bdev_0" "$aio_bdev" "$block_size")
	lvs_uuid=$(rpc_cmd bdev_lvol_get_lvstores -l lvs_test)
	uuid=$(rpc_cmd bdev_get_bdevs -b lvs_test/eclone1 | jq -r '.[].name')
	[[ "$uuid" == "$eclone_uuid" ]]
	uuid=$(rpc_cmd bdev_get_bdevs -b lvs_test/snap1 | jq -r '.[].name')
	[[ "$uuid" == "$snap_uuid" ]]
	uuid=$(rpc_cmd bdev_get_bdevs -b lvs_test/clone1 | jq -r '.[].name')
	[[ "$uuid" == "$clone_uuid" ]]

	rpc_cmd bdev_lvol_delete "$clone_uuid"
	rpc_cmd bdev_lvol_delete "$snap_uuid"
	rpc_cmd bdev_lvol_delete "$eclone_uuid"
	rpc_cmd bdev_aio_delete "$aio_bdev"
	rpc_cmd bdev_malloc_delete "$esnap_dev"
}

function log_jq_out() {
	local key

	xtrace_disable

	while read -r key; do
		printf '%50s = %s\n' "$key" "${jq_out[$key]}"
	done < <(printf '%s\n' "${!jq_out[@]}" | sort)

	xtrace_restore
}

function verify_clone() {
	local bdev=$1
	local parent=$2

	rpc_cmd_simple_data_json bdev bdev_get_bdevs -b "$bdev"
	log_jq_out

	[[ "${jq_out["supported_io_types.read"]}" == true ]]
	[[ "${jq_out["supported_io_types.write"]}" == true ]]
	[[ "${jq_out["driver_specific.lvol.clone"]}" == true ]]
	[[ "${jq_out["driver_specific.lvol.base_snapshot"]}" == "$parent" ]]
	[[ "${jq_out["driver_specific.lvol.esnap_clone"]}" == false ]]
	[[ "${jq_out["driver_specific.lvol.external_snapshot_name"]}" == null ]]
}

function verify_esnap_clone() {
	local bdev=$1
	local parent=$2
	local writable=${3:-true}

	rpc_cmd_simple_data_json bdev bdev_get_bdevs -b "$bdev"
	log_jq_out

	[[ "${jq_out["supported_io_types.read"]}" == true ]]
	[[ "${jq_out["supported_io_types.write"]}" == "$writable" ]]
	[[ "${jq_out["driver_specific.lvol.esnap_clone"]}" == true ]]
	[[ "${jq_out["driver_specific.lvol.external_snapshot_name"]}" == "$parent" ]]
}

function test_esnap_clones() {
	local bs_dev esnap_dev
	local block_size=512
	local lvs_size_mb=100
	local esnap_size_mb=1
	local lvs_cluster_size=$((16 * 1024))
	local lvs_uuid esnap_uuid
	local vol1_uuid vol2_uuid vol3_uuid vol3_uuid vol4_uuid vol5_uuid

	# Create the lvstore on a malloc device.
	bs_dev=$(rpc_cmd bdev_malloc_create $lvs_size_mb $block_size)
	lvs_uuid=$(rpc_cmd bdev_lvol_create_lvstore -c "$lvs_cluster_size" "$bs_dev" lvs_test)

	# Create a bdev that will be the external snapshot
	# State:
	#    esnap1
	esnap_uuid=2abddd12-c08d-40ad-bccf-ab131586ee4c
	esnap_dev=$(rpc_cmd bdev_malloc_create -b esnap1 -u "$esnap_uuid" "$esnap_size_mb" \
		"$block_size")

	# Create an esnap clone: vol1
	# New state:
	#    esnap1 <-- vol1(rw)
	vol1_uuid=$(rpc_cmd bdev_lvol_clone_bdev "$esnap_uuid" lvs_test vol1)
	verify_esnap_clone "$vol1_uuid" "$esnap_uuid"

	# Create a snapshot of the esnap clone: vol2
	# New state:
	#   esnap1 <-- vol2(ro) <-- vol1(rw)
	vol2_uuid=$(rpc_cmd bdev_lvol_snapshot "$vol1_uuid" vol2)
	verify_esnap_clone "$vol2_uuid" "$esnap_uuid" false
	verify_clone "$vol1_uuid" vol2

	# Delete vol2.
	# New state:
	#   esnap1 <-- vol1(rw)
	rpc_cmd bdev_lvol_delete "$vol2_uuid"
	NOT rpc_cmd bdev_get_bdevs -b "$vol2_uuid"
	verify_esnap_clone "$vol1_uuid" "$esnap_uuid"
	vol2_uuid=

	# Snapshot vol1: vol3
	# New state:
	#   ensap1 <-- vol3(ro) <-- vol1(rw)
	vol3_uuid=$(rpc_cmd bdev_lvol_snapshot "$vol1_uuid" vol3)
	verify_esnap_clone "$vol3_uuid" "$esnap_uuid" false
	verify_clone "$vol1_uuid" vol3

	# Delete vol1
	# New state:
	#   esnap1 <-- vol3(ro)
	rpc_cmd bdev_lvol_delete $vol1_uuid
	NOT rpc_cmd bdev_get_bdevs -b $vol1_uuid
	verify_esnap_clone "$vol3_uuid" "$esnap_uuid" false
	vol1_uuid=

	# Create clone of vol3: vol4
	# Verify vol3 is still a read-only esnap clone and vol4 is a normal clone.
	# New state:
	#   ensap1 <-- vol3(ro) <-- vol4(rw)
	vol4_uuid=$(rpc_cmd bdev_lvol_clone "$vol3_uuid" vol4)
	rpc_cmd bdev_get_bdevs -b "$vol4_uuid"
	verify_esnap_clone "$vol3_uuid" "$esnap_uuid" false
	verify_clone "$vol4_uuid" vol3

	# Create clone of vol3 (vol5).
	# New state:
	#   ensap1 <-- vol3(ro) <-- vol4(rw)
	#                      `<-- vol5(rw)
	vol5_uuid=$(rpc_cmd bdev_lvol_clone "$vol3_uuid" vol5)
	verify_esnap_clone "$vol3_uuid" "$esnap_uuid" false
	verify_clone "$vol4_uuid" vol3
	verify_clone "$vol5_uuid" vol3

	# Cannot delete vol3 because it has multiple clones
	NOT rpc_cmd bdev_lvol_delete "$vol3_uuid"

	# Delete vol4
	# New state:
	#   ensap1 <-- vol3(ro) <-- vol5(rw)
	rpc_cmd bdev_lvol_delete "$vol4_uuid"
	NOT rpc_cmd bdev_get_bdevs -b "$vol4_uuid"
	verify_esnap_clone "$vol3_uuid" "$esnap_uuid" false
	verify_clone "$vol5_uuid" vol3

	# Delete vol3.
	# New state:
	#   ensap1 <-- vol5(rw)
	rpc_cmd bdev_lvol_delete "$vol3_uuid"
	NOT rpc_cmd bdev_get_bdevs -b "$vol3_uuid"
	verify_esnap_clone "$vol5_uuid" "$esnap_uuid"

	# Delete vol5.
	# New state:
	#   esnap1
	rpc_cmd bdev_lvol_delete "$vol5_uuid"
	NOT rpc_cmd bdev_get_bdevs -b "$vol5_uuid"

	rpc_cmd bdev_malloc_delete "$bs_dev"
	rpc_cmd bdev_malloc_delete "$esnap_dev"
}

$SPDK_BIN_DIR/spdk_tgt &
spdk_pid=$!
trap 'killprocess "$spdk_pid"; rm -f "$testdir/aio_bdev_0"; exit 1' SIGINT SIGTERM SIGPIPE EXIT
waitforlisten $spdk_pid
modprobe nbd

run_test "test_esnap_reload" test_esnap_reload
run_test "test_esnap_clones" test_esnap_clones

trap - SIGINT SIGTERM SIGPIPE EXIT
killprocess $spdk_pid
rm -f "$testdir/aio_bdev_0"
