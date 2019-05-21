#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

parse_common_script_args $@

MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=512

rpc_py="$rootdir/scripts/rpc.py"

set -e

timing_enter fio
nvmftestinit
nvmfappstart "-m 0xF"

$rpc_py nvmf_create_transport -t rdma -u 8192

malloc_bdevs="$($rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE) "
malloc_bdevs+="$($rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE)"
# Create a RAID-0 bdev from two malloc bdevs
raid_malloc_bdevs="$($rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE) "
raid_malloc_bdevs+="$($rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE)"
$rpc_py construct_raid_bdev -n raid0 -s 64 -r 0 -b "$raid_malloc_bdevs"

$rpc_py nvmf_subsystem_create nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001
for malloc_bdev in $malloc_bdevs; do
	$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 "$malloc_bdev"
done
$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t rdma -a $NVMF_FIRST_TARGET_IP -s 4420

# Append the raid0 bdev into subsystem
$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 raid0

nvme connect -t rdma -n "nqn.2016-06.io.spdk:cnode1" -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"

waitforblk "nvme0n1"
waitforblk "nvme0n2"
waitforblk "nvme0n3"

$rootdir/scripts/fio.py nvmf 4096 1 write 1 1 verify
$rootdir/scripts/fio.py nvmf 4096 1 randwrite 1 1 verify
$rootdir/scripts/fio.py nvmf 4096 128 write 1 1 verify
$rootdir/scripts/fio.py nvmf 4096 128 randwrite 1 1 verify

sync

#start hotplug test case
$rootdir/scripts/fio.py nvmf 4096 1 read 10 1 &
fio_pid=$!

sleep 3
set +e

$rpc_py destroy_raid_bdev "raid0"
for malloc_bdev in $malloc_bdevs; do
	$rpc_py delete_malloc_bdev "$malloc_bdev"
done

wait $fio_pid
fio_status=$?

nvme disconnect -n "nqn.2016-06.io.spdk:cnode1" || true

if [ $fio_status -eq 0 ]; then
        echo "nvmf hotplug test: fio successful - expected failure"
        nvmfcleanup
	nvmftestfini
        exit 1
else
        echo "nvmf hotplug test: fio failed as expected"
fi
set -e

$rpc_py delete_nvmf_subsystem nqn.2016-06.io.spdk:cnode1

rm -f ./local-job0-0-verify.state
rm -f ./local-job1-1-verify.state
rm -f ./local-job2-2-verify.state

trap - SIGINT SIGTERM EXIT

nvmfcleanup
nvmftestfini
timing_exit fio
