#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../)
spdk_nvme_cli="/home/sys_sgsw/nvme-cli"
rpc_py="python $rootdir/scripts/rpc.py"
source $rootdir/scripts/autotest_common.sh
source $rootdir/test/nvmf/common.sh

MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=512

set -xe

RDMA_IP_LIST=$(get_available_rdma_ips)
NVMF_FIRST_TARGET_IP=$(echo "$RDMA_IP_LIST" | head -n 1)
if [ -z $NVMF_FIRST_TARGET_IP ]; then
        echo "no NIC for nvmf test"
        exit 0
fi
subsystemname=nqn.2016-06.io.spdk:cnode1

timing_enter spdk_nvme_cli

timing_enter start_nvmf_tgt

start_stub "-s 2048 -i 0 -m $NVMF_TEST_CORE_MASK"
trap "kill_stub; exit 1" SIGINT SIGTERM EXIT

$rootdir/app/nvmf_tgt/nvmf_tgt -c $testdir/../nvmf/nvmf.conf -i 0 &
nvmfpid=$!
trap "killprocess $nvmfpid; exit 1" SIGINT SIGTERM EXIT
waitforlisten $nvmfpid

timing_exit start_nvmf_tgt

modprobe -v nvme-rdma

bdevs="$bdevs $($rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE)"
$rpc_py construct_nvmf_subsystem $subsystemname "trtype:RDMA traddr:$NVMF_FIRST_TARGET_IP trsvcid:4420" '' -a -s SPDK00000000000001 -n "$bdevs"
bdfs=$(lspci -mm -n | grep 0108 | tr -d '"' | awk -F " " '{print "0000:"$1}')

# Test spdk/nvme-cli mian commands:
if [ -d  $spdk_nvme_cli ]; then
	# Test spdk/nvme-cli discover,connect and disconnect commamd:
	cd $spdk_nvme_cli && ./nvme discover -t rdma -a $NVMF_FIRST_TARGET_IP -s 4420
	./nvme connect -t rdma -n "$subsystemname" -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"
	./nvme disconnect -n "$subsystemname"
	$rpc_py delete_nvmf_subsystem $subsystemname

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

	# echo 'hello world' | ./nvme write $bdfs --data-size=520 --block-count=0
	#./nvme show-regs $bdfs

	# P3700 not support create-ns,delete-ns,attach-ns,detach-ns,resv-report,subsystem-reset,compare,Security Send/Receive ...
	# So,some nvme command can't be test:

	#./nvme resv-report $bdfs
	#./nvme admin-passthru /dev/nvme0n1 -o 1 -f 1 -p 0 -R 1 -n 1 -m 1 -t 10 -2 1 -i $spdk_nvme_cli/test.txt -b -s -d -r
	#./nvme create-ns $bdfs -s 0x1400000 -c 0x1400000 -f 0 -d 0 -m 0
	#./nvme attach-ns $bdfs -n 1
	#./nvme delete-ns $bdfs -n 1
	#./nvme detach-ns $bdfs -n 1
	#./nvme dsm -n 1 $bdfs
	#./nvme subsystem-reset $bdf


fi

trap "exit 1" SIGINT SIGTERM EXIT

kill_stub
nvmfcleanup
killprocess $nvmfpid
timing_exit spdk_nvme_cli
