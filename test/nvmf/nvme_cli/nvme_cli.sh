#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

if [ -z "${DEPENDENCY_DIR}" ]; then
        echo DEPENDENCY_DIR not defined!
        exit 1
fi

spdk_nvme_cli="${DEPENDENCY_DIR}/nvme-cli"

MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=512

rpc_py="$rootdir/scripts/rpc.py"

set -e

# pass the parameter 'iso' to this script when running it in isolation to trigger rdma device initialization.
# e.g. sudo ./nvme_cli.sh iso
nvmftestinit $1

RDMA_IP_LIST=$(get_available_rdma_ips)
NVMF_FIRST_TARGET_IP=$(echo "$RDMA_IP_LIST" | head -n 1)
if [ -z $NVMF_FIRST_TARGET_IP ]; then
	echo "no NIC for nvmf test"
	exit 0
fi

timing_enter nvme_cli
timing_enter start_nvmf_tgt
$NVMF_APP -m 0xF &
nvmfpid=$!

trap "process_shm --id $NVMF_APP_SHM_ID; killprocess $nvmfpid; nvmftestfini $1; exit 1" SIGINT SIGTERM EXIT

waitforlisten $nvmfpid
$rpc_py nvmf_create_transport -t RDMA -u 8192 -p 4
timing_exit start_nvmf_tgt

bdevs="$($rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE) "
bdevs+="$($rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE)"

modprobe -v nvme-rdma

$rpc_py nvmf_subsystem_create nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001
for bdev in $bdevs; do
	$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 $bdev
done
$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t rdma -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT

nvme connect -t rdma -n "nqn.2016-06.io.spdk:cnode1" -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"

waitforblk "nvme0n1"
waitforblk "nvme0n2"

nvme list

for ctrl in /dev/nvme?; do
	nvme id-ctrl $ctrl
	nvme smart-log $ctrl
done

for ns in /dev/nvme?n*; do
	nvme id-ns $ns
done

nvme disconnect -n "nqn.2016-06.io.spdk:cnode1" || true
nvme disconnect -n "nqn.2016-06.io.spdk:cnode2" || true

if [ -d  $spdk_nvme_cli ]; then
	# Test spdk/nvme-cli NVMe-oF commands: discover, connect and disconnect
	cd $spdk_nvme_cli
	./nvme discover -t rdma -a $NVMF_FIRST_TARGET_IP -s "$NVMF_PORT"
	nvme_num_before_connection=$(nvme list |grep "/dev/nvme*"|awk '{print $1}'|wc -l)
	./nvme connect -t rdma -n "nqn.2016-06.io.spdk:cnode1" -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"
	sleep 1
	nvme_num=$(nvme list |grep "/dev/nvme*"|awk '{print $1}'|wc -l)
	./nvme disconnect -n "nqn.2016-06.io.spdk:cnode1"
	if [ $nvme_num -le $nvme_num_before_connection ]; then
		echo "spdk/nvme-cli connect target devices failed"
		exit 1
	fi
fi

$rpc_py delete_nvmf_subsystem nqn.2016-06.io.spdk:cnode1
trap - SIGINT SIGTERM EXIT

nvmfcleanup
killprocess $nvmfpid
nvmftestfini $1
report_test_completion "nvmf_spdk_nvme_cli"
timing_exit nvme_cli
