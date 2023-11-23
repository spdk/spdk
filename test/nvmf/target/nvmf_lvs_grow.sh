#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2023 Intel Corporation
#  All rights reserved.
#
testdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$testdir/../../..")
source "$rootdir/test/common/autotest_common.sh"
source "$rootdir/test/nvmf/common.sh"

rpc_py="$rootdir/scripts/rpc.py"
bdevperf_rpc_sock="/var/tmp/bdevperf.sock"

lvs_grow() {
	local aio_bdev lvs lvol
	local data_clusters free_clusters
	local bdevperf_pid run_test_pid
	local aio_init_size_mb=200
	local aio_final_size_mb=400
	local lvol_bdev_size_mb=150

	# Create an AIO bdev for the logical volume store
	rm -f "$testdir/aio_bdev"
	truncate -s "${aio_init_size_mb}M" "$testdir/aio_bdev"
	aio_bdev=$($rpc_py bdev_aio_create "$testdir/aio_bdev" aio_bdev 4096)

	# Create the logical volume store on the AIO bdev, with predictable cluster size and remaining md pages for grow
	lvs=$($rpc_py bdev_lvol_create_lvstore --cluster-sz 4194304 --md-pages-per-cluster-ratio 300 $aio_bdev lvs)
	data_clusters=$($rpc_py bdev_lvol_get_lvstores -u $lvs | jq -r '.[0].total_data_clusters')
	((data_clusters == 49))

	# Create a thin provisioned logical volume on the logical volume store
	lvol=$($rpc_py bdev_lvol_create -u $lvs lvol $lvol_bdev_size_mb)

	# Increase the AIO file size, without yet increasing the logical volume store size
	truncate -s "${aio_final_size_mb}M" "$testdir/aio_bdev"
	$rpc_py bdev_aio_rescan $aio_bdev
	((data_clusters == $($rpc_py bdev_lvol_get_lvstores -u $lvs | jq -r '.[0].total_data_clusters')))

	# Create an NVMe-oF subsystem and add the logical volume as a namespace
	$rpc_py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode0 -a -s SPDK0
	$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode0 $lvol
	$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode0 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT
	$rpc_py nvmf_subsystem_add_listener discovery -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT

	# Start random writes in the background
	$SPDK_EXAMPLE_DIR/bdevperf -r $bdevperf_rpc_sock -m 0x2 -o 4096 -q 128 -w randwrite -t 10 -S 1 -z &
	bdevperf_pid=$!
	trap 'killprocess $bdevperf_pid; nvmftestfini; exit 1' SIGINT SIGTERM EXIT
	waitforlisten $bdevperf_pid $bdevperf_rpc_sock

	$rpc_py -s $bdevperf_rpc_sock bdev_nvme_attach_controller -b Nvme0 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT -f ipv4 -n nqn.2016-06.io.spdk:cnode0
	$rpc_py -s $bdevperf_rpc_sock bdev_get_bdevs -b Nvme0n1 -t 3000

	$rootdir/examples/bdev/bdevperf/bdevperf.py -s $bdevperf_rpc_sock perform_tests &
	run_test_pid=$!
	sleep 2

	# Perform grow operation on the logical volume store
	$rpc_py bdev_lvol_grow_lvstore -u $lvs
	data_clusters=$($rpc_py bdev_lvol_get_lvstores -u $lvs | jq -r '.[0].total_data_clusters')
	((data_clusters == 99))

	# Wait for I/O to complete
	wait $run_test_pid
	killprocess $bdevperf_pid

	$rpc_py nvmf_delete_subsystem nqn.2016-06.io.spdk:cnode0
	free_clusters=$($rpc_py bdev_lvol_get_lvstores -u $lvs | jq -r '.[0].free_clusters')

	if [[ "$1" == "dirty" ]]; then
		# Immediately shutdown nvmf_tgt without a chance to persist metadata after grow
		kill -9 $nvmfpid
		wait $nvmfpid || true
		nvmfappstart -m 0x1
		aio_bdev=$($rpc_py bdev_aio_create "$testdir/aio_bdev" aio_bdev 4096)
		waitforbdev $lvol
		((free_clusters == $($rpc_py bdev_lvol_get_lvstores -u $lvs | jq -r '.[0].free_clusters')))
		((data_clusters == $($rpc_py bdev_lvol_get_lvstores -u $lvs | jq -r '.[0].total_data_clusters')))
	fi

	# Reload the logical volume store, making sure that number of clusters remain unchanged
	$rpc_py bdev_aio_delete $aio_bdev
	NOT $rpc_py bdev_lvol_get_lvstores -u $lvs
	$rpc_py bdev_aio_create "$testdir/aio_bdev" $aio_bdev 4096
	waitforbdev $lvol
	((free_clusters == $($rpc_py bdev_lvol_get_lvstores -u $lvs | jq -r '.[0].free_clusters')))
	((data_clusters == $($rpc_py bdev_lvol_get_lvstores -u $lvs | jq -r '.[0].total_data_clusters')))

	# Clean up
	$rpc_py bdev_lvol_delete $lvol
	$rpc_py bdev_lvol_delete_lvstore -u $lvs
	$rpc_py bdev_aio_delete $aio_bdev
	rm -f "$testdir/aio_bdev"
}

nvmftestinit
nvmfappstart -m 0x1
$rpc_py nvmf_create_transport $NVMF_TRANSPORT_OPTS -u 8192

run_test "lvs_grow_clean" lvs_grow
run_test "lvs_grow_dirty" lvs_grow dirty
