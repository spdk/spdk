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

function test_esnap_reload_missing() {
	local bs_dev esnap_dev
	local block_size=512
	local esnap_size_mb=1
	local lvs_cluster_size=$((16 * 1024))
	local lvs_uuid esnap_uuid eclone_uuid snap_uuid clone_uuid uuid
	local aio_bdev=test_esnap_reload_aio0
	local lvols

	# Create the lvstore on an aio device. Can't use malloc because we need to remove
	# the device and re-add it to trigger an lvstore unload and then load.
	rm -f $testdir/aio_bdev_0
	truncate -s "${AIO_SIZE_MB}M" $testdir/aio_bdev_0
	bs_dev=$(rpc_cmd bdev_aio_create "$testdir/aio_bdev_0" "$aio_bdev" "$block_size")
	lvs_uuid=$(rpc_cmd bdev_lvol_create_lvstore -c "$lvs_cluster_size" "$bs_dev" lvs_test)

	# Create a bdev that will be the external snapshot
	# State:
	#   esnap(present) <-- eclone(not degraded)
	esnap_uuid=e4b40d8b-f623-416d-8234-baf5a4c83cbd
	esnap_dev=$(rpc_cmd bdev_malloc_create -u "$esnap_uuid" "$esnap_size_mb" "$block_size")
	eclone_uuid=$(rpc_cmd bdev_lvol_clone_bdev "$esnap_uuid" lvs_test "eclone")
	lvols=$(rpc_cmd bdev_lvol_get_lvols)
	[[ "$(jq -r '. | length' <<< "$lvols")" == "1" ]]
	[[ "$(jq -r '.[] | select(.name == "eclone").is_esnap_clone' <<< "$lvols")" == "true" ]]
	[[ "$(jq -r '.[] | select(.name == "eclone").is_degraded' <<< "$lvols")" == "false" ]]

	# Unload the lvstore and delete the external snapshot
	rpc_cmd bdev_aio_delete "$aio_bdev"
	NOT rpc_cmd bdev_lvol_get_lvstores -l lvs_test
	rpc_cmd bdev_malloc_delete "$esnap_uuid"

	# Load the lvstore, eclone bdev should not exist but the lvol should exist.
	# State:
	#   esnap(missing) <-- eclone(degraded)
	bs_dev=$(rpc_cmd bdev_aio_create "$testdir/aio_bdev_0" "$aio_bdev" "$block_size")
	NOT rpc_cmd bdev_get_bdevs -b lvs_test/eclone
	NOT rpc_cmd bdev_get_bdevs -b "$eclone_uuid"
	lvols=$(rpc_cmd bdev_lvol_get_lvols)
	[[ "$(jq -r '. | length' <<< "$lvols")" == "1" ]]
	[[ "$(jq -r '.[] | select(.name == "eclone").is_degraded' <<< "$lvols")" == "true" ]]

	# Reload the lvstore with esnap present during load. This should make the lvol not degraded.
	# State:
	#   esnap(present) <-- eclone(not degraded)
	rpc_cmd bdev_aio_delete "$aio_bdev"
	NOT rpc_cmd bdev_lvol_get_lvstores -l lvs_test
	esnap_dev=$(rpc_cmd bdev_malloc_create -u "$esnap_uuid" "$esnap_size_mb" "$block_size")
	bs_dev=$(rpc_cmd bdev_aio_create "$testdir/aio_bdev_0" "$aio_bdev" "$block_size")
	rpc_cmd bdev_lvol_get_lvstores -l lvs_test
	rpc_cmd bdev_get_bdevs -b lvs_test/eclone
	rpc_cmd bdev_get_bdevs -b "$eclone_uuid"
	lvols=$(rpc_cmd bdev_lvol_get_lvols)
	[[ "$(jq -r '. | length' <<< "$lvols")" == "1" ]]
	[[ "$(jq -r '.[] | select(.name == "eclone").is_esnap_clone' <<< "$lvols")" == "true" ]]
	[[ "$(jq -r '.[] | select(.name == "eclone").is_degraded' <<< "$lvols")" == "false" ]]

	# Create a clone of eclone, then reload without the esnap present.
	# State:
	#   esnap(missing) <-- eclone(degraded) <-- clone(degraded)
	rpc_cmd bdev_lvol_set_read_only "$eclone_uuid"
	clone_uuid=$(rpc_cmd bdev_lvol_clone "$eclone_uuid" clone)
	rpc_cmd bdev_get_bdevs -b lvs_test/clone
	rpc_cmd bdev_get_bdevs -b "$clone_uuid"
	rpc_cmd bdev_aio_delete "$aio_bdev"
	NOT rpc_cmd bdev_lvol_get_lvstores -l lvs_test
	rpc_cmd bdev_malloc_delete "$esnap_uuid"
	bs_dev=$(rpc_cmd bdev_aio_create "$testdir/aio_bdev_0" "$aio_bdev" "$block_size")
	lvols=$(rpc_cmd bdev_lvol_get_lvols)
	[[ "$(jq -r '.[] | select(.name == "eclone").is_esnap_clone' <<< "$lvols")" == "true" ]]
	[[ "$(jq -r '.[] | select(.name == "eclone").is_degraded' <<< "$lvols")" == "true" ]]
	[[ "$(jq -r '.[] | select(.name == "clone").is_clone' <<< "$lvols")" == "true" ]]
	[[ "$(jq -r '.[] | select(.name == "eclone").is_degraded' <<< "$lvols")" == "true" ]]
	NOT rpc_cmd bdev_get_bdevs -b lvs_test/eclone
	NOT rpc_cmd bdev_get_bdevs -b "$eclone_uuid"
	NOT rpc_cmd bdev_get_bdevs -b lvs_test/clone
	NOT rpc_cmd bdev_get_bdevs -b "$clone_uuid"

	# Reload the lvstore with esnap present during load. This should make the lvols not
	# degraded.
	# State:
	#   esnap(present) <-- eclone(not degraded) <-- clone(not degraded)
	rpc_cmd bdev_aio_delete "$aio_bdev"
	NOT rpc_cmd bdev_lvol_get_lvstores -l lvs_test
	esnap_dev=$(rpc_cmd bdev_malloc_create -u "$esnap_uuid" "$esnap_size_mb" "$block_size")
	bs_dev=$(rpc_cmd bdev_aio_create "$testdir/aio_bdev_0" "$aio_bdev" "$block_size")
	lvols=$(rpc_cmd bdev_lvol_get_lvols)
	[[ "$(jq -r '.[] | select(.name == "eclone").is_esnap_clone' <<< "$lvols")" == "true" ]]
	[[ "$(jq -r '.[] | select(.name == "eclone").is_degraded' <<< "$lvols")" == "false" ]]
	[[ "$(jq -r '.[] | select(.name == "clone").is_clone' <<< "$lvols")" == "true" ]]
	[[ "$(jq -r '.[] | select(.name == "clone").is_degraded' <<< "$lvols")" == "false" ]]
	rpc_cmd bdev_get_bdevs -b lvs_test/eclone
	rpc_cmd bdev_get_bdevs -b "$eclone_uuid"
	rpc_cmd bdev_get_bdevs -b lvs_test/clone
	rpc_cmd bdev_get_bdevs -b "$clone_uuid"

	# Create a snapshot of clone, then reload without the esnap present.
	# State:
	#   esnap(missing) <-- eclone(degraded) <-- snap(degraded) <-- clone(degraded)
	snap_uuid=$(rpc_cmd bdev_lvol_snapshot "$clone_uuid" snap)
	rpc_cmd bdev_get_bdevs -b lvs_test/snap
	rpc_cmd bdev_get_bdevs -b "$snap_uuid"
	rpc_cmd bdev_aio_delete "$aio_bdev"
	NOT rpc_cmd bdev_lvol_get_lvstores -l lvs_test
	rpc_cmd bdev_malloc_delete "$esnap_uuid"
	bs_dev=$(rpc_cmd bdev_aio_create "$testdir/aio_bdev_0" "$aio_bdev" "$block_size")
	lvols=$(rpc_cmd bdev_lvol_get_lvols)
	[[ "$(jq -r '.[] | select(.name == "eclone").is_esnap_clone' <<< "$lvols")" == "true" ]]
	[[ "$(jq -r '.[] | select(.name == "eclone").is_degraded' <<< "$lvols")" == "true" ]]
	[[ "$(jq -r '.[] | select(.name == "clone").is_clone' <<< "$lvols")" == "true" ]]
	[[ "$(jq -r '.[] | select(.name == "clone").is_degraded' <<< "$lvols")" == "true" ]]
	[[ "$(jq -r '.[] | select(.name == "snap").is_clone' <<< "$lvols")" == "true" ]]
	[[ "$(jq -r '.[] | select(.name == "snap").is_snapshot' <<< "$lvols")" == "true" ]]
	[[ "$(jq -r '.[] | select(.name == "snap").is_degraded' <<< "$lvols")" == "true" ]]
	NOT rpc_cmd bdev_get_bdevs -b lvs_test/eclone
	NOT rpc_cmd bdev_get_bdevs -b "$eclone_uuid"
	NOT rpc_cmd bdev_get_bdevs -b lvs_test/clone
	NOT rpc_cmd bdev_get_bdevs -b "$clone_uuid"
	NOT rpc_cmd bdev_get_bdevs -b lvs_test/snap
	NOT rpc_cmd bdev_get_bdevs -b "$snap_uuid"

	# Create the esnap bdev and verify the degraded bdevs become not degraded.
	esnap_dev=$(rpc_cmd bdev_malloc_create -u "$esnap_uuid" "$esnap_size_mb" "$block_size")
	rpc_cmd bdev_wait_for_examine
	lvols=$(rpc_cmd bdev_lvol_get_lvols)
	[[ "$(jq -r '.[] | select(.name == "eclone").is_esnap_clone' <<< "$lvols")" == "true" ]]
	[[ "$(jq -r '.[] | select(.name == "eclone").is_degraded' <<< "$lvols")" == "false" ]]
	[[ "$(jq -r '.[] | select(.name == "clone").is_clone' <<< "$lvols")" == "true" ]]
	[[ "$(jq -r '.[] | select(.name == "clone").is_degraded' <<< "$lvols")" == "false" ]]
	[[ "$(jq -r '.[] | select(.name == "snap").is_clone' <<< "$lvols")" == "true" ]]
	[[ "$(jq -r '.[] | select(.name == "snap").is_snapshot' <<< "$lvols")" == "true" ]]
	[[ "$(jq -r '.[] | select(.name == "snap").is_degraded' <<< "$lvols")" == "false" ]]
	rpc_cmd bdev_get_bdevs -b lvs_test/eclone
	rpc_cmd bdev_get_bdevs -b "$eclone_uuid"
	rpc_cmd bdev_get_bdevs -b lvs_test/clone
	rpc_cmd bdev_get_bdevs -b "$clone_uuid"
	rpc_cmd bdev_get_bdevs -b lvs_test/snap
	rpc_cmd bdev_get_bdevs -b "$snap_uuid"

	rpc_cmd bdev_aio_delete "$aio_bdev"
	rpc_cmd bdev_malloc_delete "$esnap_uuid"
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

function test_esnap_late_arrival() {
	local bs_dev esnap_dev
	local block_size=512
	local esnap_size_mb=1
	local lvs_cluster_size=$((16 * 1024))
	local lvs_uuid esnap_uuid eclone_uuid snap_uuid clone_uuid uuid
	local aio_bdev=test_esnap_reload_aio0
	local lvols

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

	# Delete the external snapshot device then reload the lvstore.
	rpc_cmd bdev_malloc_delete "$esnap_dev"
	bs_dev=$(rpc_cmd bdev_aio_create "$testdir/aio_bdev_0" "$aio_bdev" "$block_size")
	lvs_uuid=$(rpc_cmd bdev_lvol_get_lvstores -l lvs_test)

	# Verify that the esnap clone exists but does not have the esnap loaded.
	NOT rpc_cmd bdev_get_bdevs -b "$esnap_uuid"
	NOT rpc_cmd bdev_get_bdevs -b "$eclone_uuid"
	lvols=$(rpc_cmd bdev_lvol_get_lvols)
	[[ "$(jq -r '.[] | select(.uuid == "'$eclone_uuid'").is_esnap_clone' <<< "$lvols")" == "true" ]]
	[[ "$(jq -r '.[] | select(.uuid == "'$eclone_uuid'").is_degraded' <<< "$lvols")" == "true" ]]

	# Create the esnap device and verify that the esnap clone finds it.
	esnap_dev=$(rpc_cmd bdev_malloc_create -u "$esnap_uuid" "$esnap_size_mb" "$block_size")
	rpc_cmd bdev_wait_for_examine
	verify_esnap_clone "$eclone_uuid" "$esnap_uuid"

	rpc_cmd bdev_aio_delete "$aio_bdev"
	rpc_cmd bdev_malloc_delete "$esnap_dev"
}

function test_esnap_remove_degraded() {
	local bs_dev esnap_dev
	local block_size=512
	local esnap_size_mb=1
	local lvs_cluster_size=$((16 * 1024))
	local lvs_uuid esnap_uuid eclone_uuid snap_uuid clone_uuid uuid
	local aio_bdev=test_esnap_reload_aio0
	local lvols

	# Create the lvstore on an aio device. Can't use malloc because we need to remove
	# the device and re-add it to trigger an lvstore unload and then load.
	rm -f $testdir/aio_bdev_0
	truncate -s "${AIO_SIZE_MB}M" $testdir/aio_bdev_0
	bs_dev=$(rpc_cmd bdev_aio_create "$testdir/aio_bdev_0" "$aio_bdev" "$block_size")
	lvs_uuid=$(rpc_cmd bdev_lvol_create_lvstore -c "$lvs_cluster_size" "$bs_dev" lvs_test)

	# Create a bdev that will be the external snapshot
	esnap_uuid=e4b40d8b-f623-416d-8234-baf5a4c83cbd
	esnap_dev=$(rpc_cmd bdev_malloc_create -u "$esnap_uuid" "$esnap_size_mb" "$block_size")
	eclone_uuid=$(rpc_cmd bdev_lvol_clone_bdev "$esnap_uuid" lvs_test "eclone")
	rpc_cmd bdev_get_bdevs -b "$eclone_uuid"

	# Create a clone of eclone
	rpc_cmd bdev_lvol_set_read_only "$eclone_uuid"
	clone_uuid=$(rpc_cmd bdev_lvol_clone "$eclone_uuid" clone)
	rpc_cmd bdev_get_bdevs -b "$clone_uuid"

	# Reload the lvolstore without the external snapshot
	rpc_cmd bdev_aio_delete "$aio_bdev"
	NOT rpc_cmd bdev_lvol_get_lvstores -l lvs_test
	rpc_cmd bdev_malloc_delete "$esnap_dev"
	bs_dev=$(rpc_cmd bdev_aio_create "$testdir/aio_bdev_0" "$aio_bdev" "$block_size")
	lvs_uuid=$(rpc_cmd bdev_lvol_get_lvstores -l lvs_test)

	# Verify clone and eclone are degraded
	lvols=$(rpc_cmd bdev_lvol_get_lvols)
	[[ "$(jq -r '.[] | select(.uuid == "'$eclone_uuid'").is_degraded' <<< "$lvols")" == "true" ]]
	[[ "$(jq -r '.[] | select(.uuid == "'$clone_uuid'").is_degraded' <<< "$lvols")" == "true" ]]
	NOT rpc_cmd bdev_get_bdevs -b "$clone_uuid"
	NOT rpc_cmd bdev_get_bdevs -b "$eclone_uuid"

	# Delete the lvols and verify they are gone.
	rpc_cmd bdev_lvol_delete "$clone_uuid"
	lvols=$(rpc_cmd bdev_lvol_get_lvols)
	[[ "$(jq -r '. | length' <<< "$lvols")" == "1" ]]
	rpc_cmd bdev_lvol_delete "$eclone_uuid"
	lvols=$(rpc_cmd bdev_lvol_get_lvols)
	[[ "$(jq -r '. | length' <<< "$lvols")" == "0" ]]

	rpc_cmd bdev_aio_delete "$aio_bdev"
}

function test_lvol_set_parent_bdev_from_esnap() {
	local vol_size_mb=20
	local vol_size=$((vol_size_mb * 1024 * 1024))
	local vol_blocks_count=$((vol_size / MALLOC_BS))
	local three_clusters_size=$((LVS_DEFAULT_CLUSTER_SIZE * 3))
	local three_clusters_block_count=$((LVS_DEFAULT_CLUSTER_SIZE * 3 / MALLOC_BS))
	local two_clusters_block_count=$((LVS_DEFAULT_CLUSTER_SIZE * 2 / MALLOC_BS))

	# Create the lvstore on a malloc device.
	malloc_name=$(rpc_cmd bdev_malloc_create $MALLOC_SIZE_MB $MALLOC_BS)
	lvs_uuid=$(rpc_cmd bdev_lvol_create_lvstore "$malloc_name" lvs_test)

	# Create a bdev that will be the old external snapshot
	# State:
	#    esnap1
	rpc_cmd bdev_malloc_create -b esnap1 "$vol_size_mb" $MALLOC_BS

	# Perform write operation over esnap1
	nbd_start_disks "$DEFAULT_RPC_ADDR" esnap1 /dev/nbd0
	run_fio_test /dev/nbd0 0 $vol_size "write" "0xaa" ""
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd0
	sleep 1

	# Create a bdev that will be the new external snapshot
	# New state:
	#    esnap1
	#    esnap2
	esnap2_uuid=037128af-3662-4137-9e24-e74e44310ad3
	rpc_cmd bdev_malloc_create -b esnap2 -u "$esnap2_uuid" "$vol_size_mb" $MALLOC_BS

	# Perform write operation over esnap2
	# Calculate md5 of the last part of esnap2 corresponding to 2 clusters size
	nbd_start_disks "$DEFAULT_RPC_ADDR" esnap2 /dev/nbd2
	run_fio_test /dev/nbd2 0 $vol_size "write" "0xbb" ""
	md5_2=$(dd if=/dev/nbd2 bs=$MALLOC_BS count=$two_clusters_block_count skip=$three_clusters_block_count | md5sum)
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd2
	sleep 1

	# Create an esnap1 clone: lvol
	# New state:
	#    esnap1 <-- lvol
	#    esnap2
	lvol_uuid=$(rpc_cmd bdev_lvol_clone_bdev esnap1 lvs_test lvol)

	# Perform write operation over the first 3 clusters of lvol
	# Calculate md5sum of the first 3 clusters and of last 2 clusters of lvol
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$lvol_uuid" /dev/nbd2
	run_fio_test /dev/nbd2 0 $three_clusters_size "write" "0xcc" ""
	md5_lvol_1=$(dd if=/dev/nbd2 bs=$MALLOC_BS count=$three_clusters_block_count | md5sum)
	md5_lvol_2=$(dd if=/dev/nbd2 bs=$MALLOC_BS count=$two_clusters_block_count skip=$three_clusters_block_count | md5sum)
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd2

	# Change parent of lvol
	# New state:
	#    esnap1
	#    esnap2  <-- lvol
	rpc_cmd bdev_lvol_set_parent_bdev "$lvol_uuid" "$esnap2_uuid"

	# Check lvol consistency
	verify_esnap_clone "$lvol_uuid" "$esnap2_uuid"

	# Try again with aliases instead uuid
	NOT rpc_cmd bdev_lvol_set_parent_bdev lvs_test/lvol esnap2

	# Calculate again md5 of the first 3 clusters and of last 2 clusters of lvol
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$lvol_uuid" /dev/nbd2
	md5_lvol_1_new=$(dd if=/dev/nbd2 bs=$MALLOC_BS count=$three_clusters_block_count | md5sum)
	md5_lvol_2_new=$(dd if=/dev/nbd2 bs=$MALLOC_BS count=$two_clusters_block_count skip=$three_clusters_block_count | md5sum)
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd2

	# Check that first three clusters of lvol didn't change anche that the last 2 clusters changed
	[[ $md5_lvol_1 == "$md5_lvol_1_new" ]]
	[[ $md5_lvol_2 != "$md5_lvol_2_new" ]]
	[[ $md5_lvol_2_new == "$md5_2" ]]

	# Clean up
	rpc_cmd bdev_lvol_delete "$lvol_uuid"
	rpc_cmd bdev_malloc_delete esnap1
	rpc_cmd bdev_malloc_delete esnap2
	rpc_cmd bdev_lvol_delete_lvstore -u "$lvs_uuid"
	rpc_cmd bdev_malloc_delete "$malloc_name"
	check_leftover_devices
}

function test_lvol_set_parent_bdev_from_snapshot() {
	local vol_size_mb=20
	local vol_size=$((vol_size_mb * 1024 * 1024))
	local vol_blocks_count=$((vol_size / MALLOC_BS))
	local three_clusters_size=$((LVS_DEFAULT_CLUSTER_SIZE * 3))
	local three_clusters_block_count=$((LVS_DEFAULT_CLUSTER_SIZE * 3 / MALLOC_BS))
	local two_clusters_block_count=$((LVS_DEFAULT_CLUSTER_SIZE * 2 / MALLOC_BS))

	# Create the lvstore on a malloc device.
	malloc_name=$(rpc_cmd bdev_malloc_create $MALLOC_SIZE_MB $MALLOC_BS)
	lvs_uuid=$(rpc_cmd bdev_lvol_create_lvstore "$malloc_name" lvs_test)

	# Create a bdev that will be the external snapshot
	# State:
	#    esnap1
	esnap1_uuid=533c2e20-3e74-47a1-9c4f-0ffe4922ffed
	rpc_cmd bdev_malloc_create -b esnap1 -u "$esnap1_uuid" "$vol_size_mb" $MALLOC_BS

	# Perform write operation over the external snapshot
	# Calculate md5 of the last part of esnap1 corresponding to 2 clusters size
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$esnap1_uuid" /dev/nbd1
	run_fio_test /dev/nbd1 0 $vol_size "write" "0xaa" ""
	md5_1=$(dd if=/dev/nbd1 bs=$MALLOC_BS count=$two_clusters_block_count skip=$three_clusters_block_count | md5sum)
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd1
	sleep 1

	# Create a volume: lvol
	# New state:
	#    esnap1
	#    lvol
	lvol_uuid=$(rpc_cmd bdev_lvol_create -u "$lvs_uuid" lvol "$vol_size_mb")

	# Perform write operation over lvol
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$lvol_uuid" /dev/nbd2
	run_fio_test /dev/nbd2 0 $vol_size "write" "0xbb" ""
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd2

	# Make a snapshot of lvol: snap2
	# New state:
	#    esnap1
	#    snap2  <-- lvol
	snap2_uuid=$(rpc_cmd bdev_lvol_snapshot "$lvol_uuid" snap2)

	# Perform write operation over the first 3 clusters of lvol
	# Calculate md5sum of the first 3 clusters and of last 2 clusters of lvol
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$lvol_uuid" /dev/nbd2
	run_fio_test /dev/nbd2 0 $three_clusters_size "write" "0xcc" ""
	md5_lvol_1=$(dd if=/dev/nbd2 bs=$MALLOC_BS count=$three_clusters_block_count | md5sum)
	md5_lvol_2=$(dd if=/dev/nbd2 bs=$MALLOC_BS count=$two_clusters_block_count skip=$three_clusters_block_count | md5sum)
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd2

	# Change parent of lvol
	# New state:
	#    esnap1 <-- lvol
	#    snap2
	rpc_cmd bdev_lvol_set_parent_bdev "$lvol_uuid" "$esnap1_uuid"

	# Check lvol consistency
	verify_esnap_clone "$lvol_uuid" "$esnap1_uuid"

	# Try again with aliases instead uuid
	NOT rpc_cmd bdev_lvol_set_parent_bdev lvs_test/lvol esnap1

	# Calculate again md5 of the first 3 clusters and of last 2 clusters of lvol
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$lvol_uuid" /dev/nbd2
	md5_lvol_1_new=$(dd if=/dev/nbd2 bs=$MALLOC_BS count=$three_clusters_block_count | md5sum)
	md5_lvol_2_new=$(dd if=/dev/nbd2 bs=$MALLOC_BS count=$two_clusters_block_count skip=$three_clusters_block_count | md5sum)
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd2

	# Check that first three clusters of lvol didn't change anche that the last 2 clusters changed
	[[ $md5_lvol_1 == "$md5_lvol_1_new" ]]
	[[ $md5_lvol_2 != "$md5_lvol_2_new" ]]
	[[ $md5_lvol_2_new == "$md5_1" ]]

	# Clean up
	rpc_cmd bdev_lvol_delete "$lvol_uuid"
	rpc_cmd bdev_lvol_delete "$snap2_uuid"
	rpc_cmd bdev_malloc_delete esnap1
	rpc_cmd bdev_lvol_delete_lvstore -u "$lvs_uuid"
	rpc_cmd bdev_malloc_delete "$malloc_name"
	check_leftover_devices
}

function test_lvol_set_parent_bdev_from_none() {
	local vol_size_mb=20
	local vol_size=$((vol_size_mb * 1024 * 1024))
	local three_clusters_size=$((LVS_DEFAULT_CLUSTER_SIZE * 3))
	local three_clusters_block_count=$((LVS_DEFAULT_CLUSTER_SIZE * 3 / MALLOC_BS))
	local two_clusters_block_count=$((LVS_DEFAULT_CLUSTER_SIZE * 2 / MALLOC_BS))

	# Create the lvstore on a malloc device.
	malloc_name=$(rpc_cmd bdev_malloc_create $MALLOC_SIZE_MB $MALLOC_BS)
	lvs_uuid=$(rpc_cmd bdev_lvol_create_lvstore "$malloc_name" lvs_test)

	# Create a thin provisioned volume: lvol
	# New state:
	#    lvol
	lvol_uuid=$(rpc_cmd bdev_lvol_create -t -u "$lvs_uuid" lvol "$vol_size_mb")

	# Perform write operation over the first 3 clusters of lvol
	# Calculate md5sum of the first 3 clusters and of last 2 clusters of lvol
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$lvol_uuid" /dev/nbd2
	run_fio_test /dev/nbd2 0 $three_clusters_size "write" "0xaa" ""
	md5_lvol_1=$(dd if=/dev/nbd2 bs=$MALLOC_BS count=$three_clusters_block_count | md5sum)
	md5_lvol_2=$(dd if=/dev/nbd2 bs=$MALLOC_BS count=$two_clusters_block_count skip=$three_clusters_block_count | md5sum)
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd2

	# Create a bdev that will be the external snapshot
	# New State:
	#    esnap
	#    lvol
	esnap_uuid=61571088-ffcf-48d9-af1f-259eb853f7b4
	rpc_cmd bdev_malloc_create -b esnap -u "$esnap_uuid" "$vol_size_mb" $MALLOC_BS

	# Perform write operation over the external snapshot
	# Calculate md5 of the last part of esnap corresponding to 2 clusters size
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$esnap_uuid" /dev/nbd2
	run_fio_test /dev/nbd2 0 $vol_size "write" "0xbb" ""
	md5_2=$(dd if=/dev/nbd2 bs=$MALLOC_BS count=$two_clusters_block_count skip=$three_clusters_block_count | md5sum)
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd2
	sleep 1

	# Change parent of lvol
	# New state:
	#    esnap <-- lvol
	rpc_cmd bdev_lvol_set_parent_bdev "$lvol_uuid" "$esnap_uuid"

	# Check lvol consistency
	verify_esnap_clone "$lvol_uuid" "$esnap_uuid"

	# Try again with aliases instead uuid
	NOT rpc_cmd bdev_lvol_set_parent_bdev lvs_test/lvol esnap

	# Calculate again md5 of the first 3 clusters and of last 2 clusters of lvol
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$lvol_uuid" /dev/nbd2
	md5_lvol_1_new=$(dd if=/dev/nbd2 bs=$MALLOC_BS count=$three_clusters_block_count | md5sum)
	md5_lvol_2_new=$(dd if=/dev/nbd2 bs=$MALLOC_BS count=$two_clusters_block_count skip=$three_clusters_block_count | md5sum)
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd2

	# Check that first three clusters of lvol didn't change anche that the last 2 clusters changed
	[[ $md5_lvol_1 == "$md5_lvol_1_new" ]]
	[[ $md5_lvol_2 != "$md5_lvol_2_new" ]]
	[[ $md5_lvol_2_new == "$md5_2" ]]

	# Clean up
	rpc_cmd bdev_lvol_delete "$lvol_uuid"
	rpc_cmd bdev_malloc_delete esnap
	rpc_cmd bdev_lvol_delete_lvstore -u "$lvs_uuid"
	rpc_cmd bdev_malloc_delete "$malloc_name"
	check_leftover_devices
}

$SPDK_BIN_DIR/spdk_tgt &
spdk_pid=$!
trap 'killprocess "$spdk_pid"; rm -f "$testdir/aio_bdev_0"; exit 1' SIGINT SIGTERM SIGPIPE EXIT
waitforlisten $spdk_pid
modprobe nbd

run_test "test_esnap_reload" test_esnap_reload
run_test "test_esnap_reload" test_esnap_reload_missing
run_test "test_esnap_clones" test_esnap_clones
run_test "test_esnap_late_arrival" test_esnap_late_arrival
run_test "test_esnap_remove_degraded" test_esnap_remove_degraded
run_test "test_lvol_set_parent_bdev_from_esnap" test_lvol_set_parent_bdev_from_esnap
run_test "test_lvol_set_parent_bdev_from_snapshot" test_lvol_set_parent_bdev_from_snapshot
run_test "test_lvol_set_parent_bdev_from_none" test_lvol_set_parent_bdev_from_none

trap - SIGINT SIGTERM SIGPIPE EXIT
killprocess $spdk_pid
rm -f "$testdir/aio_bdev_0"
