#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/scripts/autotest_common.sh
source $rootdir/test/nvmf/common.sh

set -e

if ! rdma_nic_available; then
	echo "no NIC for nvmf test"
	exit 0
fi

timing_enter fio

# Start up the NVMf target in another process
$rootdir/app/nvmf_tgt/nvmf_tgt -c $testdir/../nvmf.conf -t nvmf -t rdma &
nvmfpid=$!

trap "killprocess $nvmfpid; exit 1" SIGINT SIGTERM EXIT

sleep 10

modprobe -v nvme-rdma

if [ -e "/dev/nvme-fabrics" ]; then
	chmod a+rw /dev/nvme-fabrics
fi

echo 'traddr='$NVMF_FIRST_TARGET_IP',transport=rdma,nr_io_queues=1,trsvcid='$NVMF_PORT',nqn=nqn.2016-06.io.spdk:cnode1' > /dev/nvme-fabrics

$testdir/nvmf_fio.py 4096 1 rw 1 verify
$testdir/nvmf_fio.py 4096 1 randrw 1 verify
$testdir/nvmf_fio.py 4096 128 rw 1 verify
$testdir/nvmf_fio.py 4096 128 randrw 1 verify

rm -f ./local-job0-0-verify.state

trap - SIGINT SIGTERM EXIT

nvmfcleanup
killprocess $nvmfpid
timing_exit fio
