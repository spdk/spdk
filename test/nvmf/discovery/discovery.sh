#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$testdir/../../..
source $rootdir/scripts/autotest_common.sh
source $rootdir/test/nvmf/common.sh

if ! hash nvme; then
	echo "nvme command not found; skipping discovery test"
	exit 0
fi

rdma_device_init

timing_enter discovery

# Start up the NVMf target in another process
$rootdir/app/nvmf_tgt/nvmf_tgt -c $testdir/../nvmf.conf -t nvmf -t rdma &
nvmfpid=$!

trap "process_core; killprocess $nvmfpid; exit 1" SIGINT SIGTERM EXIT

sleep 10

modprobe -v nvme-rdma

if [ -e "/dev/nvme-fabrics" ]; then
	chmod a+rw /dev/nvme-fabrics
fi

nvme discover -t rdma -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT

trap - SIGINT SIGTERM EXIT

nvmfcleanup
killprocess $nvmfpid
timing_exit discovery
