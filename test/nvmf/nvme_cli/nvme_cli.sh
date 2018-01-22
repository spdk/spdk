#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/scripts/autotest_common.sh
source $rootdir/test/nvmf/common.sh
spdk_nvme_cli="/home/sys_sgsw/nvme-cli"

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

timing_enter nvme_cli
timing_enter start_nvmf_tgt
$NVMF_APP -c $testdir/../nvmf.conf &
nvmfpid=$!

trap "killprocess $nvmfpid; exit 1" SIGINT SIGTERM EXIT

waitforlisten $nvmfpid
timing_exit start_nvmf_tgt

bdevs="$bdevs $($rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE)"
bdevs="$bdevs $($rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE)"

modprobe -v nvme-rdma

$rpc_py construct_nvmf_subsystem nqn.2016-06.io.spdk:cnode1 "trtype:RDMA traddr:$NVMF_FIRST_TARGET_IP trsvcid:4420" '' -a -s SPDK00000000000001 -n "$bdevs"

nvme connect -t rdma -n "nqn.2016-06.io.spdk:cnode1" -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"

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
	bdfs=$(lspci -mm -n | grep 0108 | tr -d '"' | awk -F " " '{print "0000:"$1}')
	# Test spdk/nvme-cli discover,connect and disconnect commamd:
	cd $spdk_nvme_cli && ./nvme discover -t rdma -a $NVMF_FIRST_TARGET_IP -s 4420
	./nvme connect -t rdma -n "nqn.2016-06.io.spdk:cnode1" -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"
	./nvme disconnect -n "nqn.2016-06.io.spdk:cnode1"
	$rpc_py delete_nvmf_subsystem "nqn.2016-06.io.spdk:cnode1"
	trap - SIGINT SIGTERM EXIT
	# Kill stub and nvmf_tgt to avoid $bdf is claimed
	nvmfcleanup
	killprocess $nvmfpid
	ps -aux|grep vim |grep -v grep|awk '{ print $2 }'|xargs kill -9
	./nvme help
	./nvme list
	./nvme id-ctrl $bdfs
	./nvme list-ctrl $bdfs
	./nvme get-ns-id $bdfs
	./nvme id-ns $bdfs
	./nvme fw-log $bdfs
	./nvme smart-log $bdfs
	./nvme error-log $bdfs
	./nvme list-ns $bdfs -n 1
	./nvme flush $bdfs -n 1
	./nvme get-feature $bdfs -n 1 -f 1 -s 1 -l 100
	./nvme fw-activate $bdfs -s 1 -a 2
	./nvme get-log $bdfs -n 1 -i 1 -l 100
	./nvme read $bdfs -s 0x0 -c 0x1 -z 1024
	./nvme write-uncor $bdfs -n 1 -s 64 -c 1
	./nvme reset $bdfs
	./nvme gen-hostnqn
else
	$rpc_py delete_nvmf_subsystem nqn.2016-06.io.spdk:cnode1
	trap - SIGINT SIGTERM EXIT

	nvmfcleanup
	killprocess $nvmfpid
fi
timing_exit nvme_cli
