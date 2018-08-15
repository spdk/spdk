#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=512

rpc_py="python $rootdir/scripts/rpc.py"

set -e

RDMA_IP_LIST=$(get_available_rdma_ips)
NVMF_FIRST_TARGET_IP=$(echo "$RDMA_IP_LIST" | head -n 1)
if [ -z $NVMF_FIRST_TARGET_IP ]; then
	echo "no NIC for nvmf test"
	exit 0
fi

timing_enter perf
timing_enter start_nvmf_tgt

$NVMF_APP -m 0xF -s 1024 --wait-for-rpc -i 0 &
nvmfpid=$!

trap "killprocess $nvmfpid; exit 1" SIGINT SIGTERM EXIT

waitforlisten $nvmfpid
$rpc_py set_nvmf_target_options -u 8192 -p 4
$rpc_py start_subsystem_init
$rootdir/scripts/gen_nvme.sh --json | $rpc_py load_subsystem_config
timing_exit start_nvmf_tgt

local_nvme_trid="trtype:PCIe traddr:"$($rpc_py get_subsystem_config bdev | jq -r '.[].params | select(.name=="Nvme0").traddr')
bdevs="$bdevs $($rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE)"

if [ -n "$local_nvme_trid" ]; then
	bdevs="$bdevs Nvme0n1"
fi

$rpc_py construct_nvmf_subsystem nqn.2016-06.io.spdk:cnode1 "trtype:RDMA traddr:$NVMF_FIRST_TARGET_IP trsvcid:4420" '' -a -s SPDK00000000000001 -n "$bdevs"

# Test multi-process access to local NVMe device
if [ -n "$local_nvme_trid" ]; then
	$rootdir/examples/nvme/perf/perf -i 0 -q 32 -s 4096 -w randrw -M 50 -t 1 -r "$local_nvme_trid"
fi

$rootdir/examples/nvme/perf/perf -q 32 -s 4096 -w randrw -M 50 -t 1 -r "trtype:RDMA adrfam:IPv4 traddr:$NVMF_FIRST_TARGET_IP trsvcid:4420"
sync
$rpc_py delete_nvmf_subsystem nqn.2016-06.io.spdk:cnode1

if [ $RUN_NIGHTLY -eq 1 ]; then
	# Configure nvme devices with nvmf lvol_bdev backend
	if [ -n "$local_nvme_trid" ]; then
		ls_guid=$($rpc_py construct_lvol_store Nvme0n1 lvs_0)
		get_lvs_free_mb $ls_guid
		lb_guid=$($rpc_py construct_lvol_bdev -u $ls_guid lbd_0 $free_mb)
		$rpc_py construct_nvmf_subsystem nqn.2016-06.io.spdk:cnode1 "trtype:RDMA traddr:$NVMF_FIRST_TARGET_IP trsvcid:4420" "" -a -s SPDK00000000000001 -n "$lb_guid"

		# Test perf as host with different io_size and qd_depth in nightly
		qd_depth=("1" "128")
		io_size=("512" "131072")
		for qd in ${qd_depth[@]}; do
			for s in ${io_size[@]}; do
				$rootdir/examples/nvme/perf/perf -q $qd -s $s -w randrw -M 50 -t 10 -r "trtype:RDMA adrfam:IPv4 traddr:$NVMF_FIRST_TARGET_IP trsvcid:4420"
			done
		done

		# Delete subsystems, lvol_bdev and destroy lvol_store.
		$rpc_py delete_nvmf_subsystem nqn.2016-06.io.spdk:cnode1
		$rpc_py destroy_lvol_bdev "$lb_guid"
		$rpc_py destroy_lvol_store -l lvs_0
		$rpc_py delete_nvme_controller Nvme0
	fi
fi

$rpc_py construct_malloc_bdev 512 512 -b Malloc1
$rpc_py construct_nvmf_subsystem -s SPDK0 nqn.2014-08.org.spdk:cnode1 "trtype:RDMA traddr:$NVMF_FIRST_TARGET_IP trsvcid:4420" '' -a -n "Malloc1"
$rootdir/examples/nvme/perf/perf -r "trtype:RDMA adrfam:IPv4 traddr:$NVMF_FIRST_TARGET_IP trsvcid:4420" -d 1024 -q 128 -s 4096 -w randread -t 10 -c 0x2 &
perfpid=$!
sleep 5

trap - SIGINT SIGTERM EXIT

killprocess $nvmfpid
killprocess $perfpid
timing_exit perf
