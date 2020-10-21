#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=512

rpc_py="$rootdir/scripts/rpc.py"

nvmftestinit
nvmfappstart -m 0xF

$rootdir/scripts/gen_nvme.sh | $rpc_py load_subsystem_config

local_nvme_trid="trtype:PCIe traddr:"$($rpc_py framework_get_config bdev | jq -r '.[].params | select(.name=="Nvme0").traddr')
bdevs="$bdevs $($rpc_py bdev_malloc_create $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE)"

if [ -n "$local_nvme_trid" ]; then
	bdevs="$bdevs Nvme0n1"
fi

$rpc_py nvmf_create_transport $NVMF_TRANSPORT_OPTS
$rpc_py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001
for bdev in $bdevs; do
	$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 $bdev
done
$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT

# Test multi-process access to local NVMe device
if [ -n "$local_nvme_trid" ]; then
	if [ $SPDK_RUN_NON_ROOT -eq 1 ]; then
		perf_app="sudo -u $USER $SPDK_EXAMPLE_DIR/perf"
	else
		perf_app="$SPDK_EXAMPLE_DIR/perf"
	fi
	$perf_app -i $NVMF_APP_SHM_ID -q 32 -o 4096 -w randrw -M 50 -t 1 -r "$local_nvme_trid"
fi

$SPDK_EXAMPLE_DIR/perf -q 1 -o 4096 -w randrw -M 50 -t 1 -r "trtype:$TEST_TRANSPORT adrfam:IPv4 traddr:$NVMF_FIRST_TARGET_IP trsvcid:$NVMF_PORT"
$SPDK_EXAMPLE_DIR/perf -q 32 -o 4096 -w randrw -M 50 -t 1 -r "trtype:$TEST_TRANSPORT adrfam:IPv4 traddr:$NVMF_FIRST_TARGET_IP trsvcid:$NVMF_PORT"
$SPDK_EXAMPLE_DIR/perf -q 128 -o 262144 -w randrw -M 50 -t 2 -r "trtype:$TEST_TRANSPORT adrfam:IPv4 traddr:$NVMF_FIRST_TARGET_IP trsvcid:$NVMF_PORT"
sync
$rpc_py nvmf_delete_subsystem nqn.2016-06.io.spdk:cnode1

if [ $RUN_NIGHTLY -eq 1 ]; then
	# Configure nvme devices with nvmf lvol_bdev backend
	if [ -n "$local_nvme_trid" ]; then
		ls_guid=$($rpc_py bdev_lvol_create_lvstore Nvme0n1 lvs_0)
		get_lvs_free_mb $ls_guid
		# We don't need to create an lvol larger than 20G for this test.
		# decreasing the size of the nested lvol allows us to take less time setting up
		#before running I/O.
		if [ $free_mb -gt 20480 ]; then
			free_mb=20480
		fi
		lb_guid=$($rpc_py bdev_lvol_create -u $ls_guid lbd_0 $free_mb)

		# Create lvol bdev for nested lvol stores
		ls_nested_guid=$($rpc_py bdev_lvol_create_lvstore $lb_guid lvs_n_0)
		get_lvs_free_mb $ls_nested_guid
		if [ $free_mb -gt 20480 ]; then
			free_mb=20480
		fi
		lb_nested_guid=$($rpc_py bdev_lvol_create -u $ls_nested_guid lbd_nest_0 $free_mb)
		$rpc_py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001
		for bdev in $lb_nested_guid; do
			$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 $bdev
		done
		$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT
		# Test perf as host with different io_size and qd_depth in nightly
		qd_depth=("1" "32" "128")
		io_size=("512" "131072")
		for qd in "${qd_depth[@]}"; do
			for o in "${io_size[@]}"; do
				$SPDK_EXAMPLE_DIR/perf -q $qd -o $o -w randrw -M 50 -t 10 -r "trtype:$TEST_TRANSPORT adrfam:IPv4 traddr:$NVMF_FIRST_TARGET_IP trsvcid:$NVMF_PORT"
			done
		done

		# Delete subsystems, lvol_bdev and destroy lvol_store.
		$rpc_py nvmf_delete_subsystem nqn.2016-06.io.spdk:cnode1
		$rpc_py bdev_lvol_delete "$lb_nested_guid"
		$rpc_py bdev_lvol_delete_lvstore -l lvs_n_0
		$rpc_py bdev_lvol_delete "$lb_guid"
		$rpc_py bdev_lvol_delete_lvstore -l lvs_0
	fi
fi

trap - SIGINT SIGTERM EXIT

nvmftestfini
