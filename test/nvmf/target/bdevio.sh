#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

rpc_py="$rootdir/scripts/rpc.py"

set -e

timing_enter bdevio
# pass the parameter 'iso' to this script when running it in isolation to trigger rdma device initialization.
# e.g. sudo ./bdev_io_wait.sh iso
nvmftestinit
nvmfappstart "-m 0xF"

$rpc_py nvmf_create_transport -t rdma -u 8192 -p 4
echo -e "$(create_malloc_nvmf_subsystem 1 rdma)" | $rpc_py

echo "[Nvme]" > $testdir/bdevperf.conf
echo "  TransportID \"trtype:rdma adrfam:IPv4 subnqn:nqn.2016-06.io.spdk:cnode1 traddr:$NVMF_FIRST_TARGET_IP trsvcid:4420\" Nvme0" >> $testdir/bdevperf.conf

$rootdir/test/bdev/bdevio/bdevio -c $testdir/bdevperf.conf

rm -rf $testdir/bdevperf.conf
$rpc_py delete_nvmf_subsystem nqn.2016-06.io.spdk:cnode1

trap - SIGINT SIGTERM EXIT

nvmfcleanup
nvmftestfini
timing_exit bdev_io_wait
