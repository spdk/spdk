#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

rpc_py="$rootdir/scripts/rpc.py"

set -e

# connect disconnect is geared towards ensuring that we are properly freeing resources after disconnecting qpairs.
timing_enter connect_disconnect

# pass the parameter 'iso' to this script when running it in isolation to trigger rdma device initialization.
# e.g. sudo ./filesystem.sh iso
nvmftestinit
nvmfappstart "-m 0xF"

$rpc_py nvmf_create_transport -t rdma -u 8192 -p 4 -c 0

echo -e "$(create_malloc_nvmf_subsystem 1 rdma)" | $rpc_py

if [ $RUN_NIGHTLY -eq 1 ]; then
	num_iterations=200
else
	num_iterations=10
fi

set +x
for i in $(seq 1 $num_iterations); do
	nvme connect -t rdma -n "nqn.2016-06.io.spdk:cnode1" -a "$NVMF_FIRST_TARGET_IP" -s "4420"
	waitforblk "nvme0n1"
	nvme disconnect -n "nqn.2016-06.io.spdk:cnode1"
	waitforblk_disconnect "nvme0n1"
done
set -x

trap - SIGINT SIGTERM EXIT

nvmfcleanup
nvmftestfini
timing_exit connect_disconnect
