#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

NVMF_SUBSYS=11

rpc_py="$rootdir/scripts/rpc.py"

set -e

timing_enter multiconnection
# pass the parameter 'iso' to this script when running it in isolation to trigger rdma device initialization.
# e.g. sudo ./multiconnection.sh iso
nvmftestinit
nvmfappstart "-m 0xF"

# SoftRoce does not have enough queues available for
# multiconnection tests. Detect if we're using software RDMA.
# If so - lower the number of subsystems for test.
if check_ip_is_soft_roce $NVMF_FIRST_TARGET_IP; then
	echo "Using software RDMA, lowering number of NVMeOF subsystems."
	NVMF_SUBSYS=1
fi

$rpc_py nvmf_create_transport -t rdma -u 8192 -p 4

for i in `seq 1 $NVMF_SUBSYS`
do
	echo -e "$(create_malloc_nvmf_subsystem $i rdma)" | $rpc_py
done

for i in `seq 1 $NVMF_SUBSYS`; do
	k=$[$i-1]
	nvme connect -t rdma -n "nqn.2016-06.io.spdk:cnode${i}" -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"

	waitforblk "nvme${k}n1"
done

$rootdir/scripts/fio.py nvmf 262144 64 read 10 1
$rootdir/scripts/fio.py nvmf 262144 64 randwrite 10 1

sync
for i in `seq 1 $NVMF_SUBSYS`; do
	nvme disconnect -n "nqn.2016-06.io.spdk:cnode${i}" || true
	$rpc_py delete_nvmf_subsystem nqn.2016-06.io.spdk:cnode${i}
done

rm -f ./local-job0-0-verify.state

trap - SIGINT SIGTERM EXIT

nvmfcleanup
nvmftestfini
timing_exit multiconnection
