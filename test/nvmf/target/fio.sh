#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=512

rpc_py="$rootdir/scripts/rpc.py"

nvmftestinit
nvmfappstart -m 0xF

$rpc_py nvmf_create_transport $NVMF_TRANSPORT_OPTS -u 8192

malloc_bdevs="$($rpc_py bdev_malloc_create $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE) "
malloc_bdevs+="$($rpc_py bdev_malloc_create $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE)"
# Create a RAID-0 bdev from two malloc bdevs
raid_malloc_bdevs="$($rpc_py bdev_malloc_create $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE) "
raid_malloc_bdevs+="$($rpc_py bdev_malloc_create $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE)"
$rpc_py bdev_raid_create -n raid0 -z 64 -r 0 -b "$raid_malloc_bdevs"

$rpc_py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a -s $NVMF_SERIAL
for malloc_bdev in $malloc_bdevs; do
	$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 "$malloc_bdev"
done
$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT

# Append the raid0 bdev into subsystem
$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 raid0

nvme connect -t $TEST_TRANSPORT -n "nqn.2016-06.io.spdk:cnode1" -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"

waitforserial $NVMF_SERIAL 3

$rootdir/scripts/fio-wrapper -p nvmf -i 4096 -d 1 -t write -r 1 -v
$rootdir/scripts/fio-wrapper -p nvmf -i 4096 -d 1 -t randwrite -r 1 -v
$rootdir/scripts/fio-wrapper -p nvmf -i 4096 -d 128 -t write -r 1 -v
$rootdir/scripts/fio-wrapper -p nvmf -i 4096 -d 128 -t randwrite -r 1 -v

sync

#start hotplug test case
$rootdir/scripts/fio-wrapper -p nvmf -i 4096 -d 1 -t read -r 10 &
fio_pid=$!

sleep 3

$rpc_py bdev_raid_delete "raid0"
for malloc_bdev in $malloc_bdevs; do
	$rpc_py bdev_malloc_delete "$malloc_bdev"
done

fio_status=0
wait $fio_pid || fio_status=$?

nvme disconnect -n "nqn.2016-06.io.spdk:cnode1" || true

if [ $fio_status -eq 0 ]; then
	echo "nvmf hotplug test: fio successful - expected failure"
	nvmftestfini
	exit 1
else
	echo "nvmf hotplug test: fio failed as expected"
fi

$rpc_py nvmf_delete_subsystem nqn.2016-06.io.spdk:cnode1

rm -f ./local-job0-0-verify.state
rm -f ./local-job1-1-verify.state
rm -f ./local-job2-2-verify.state

trap - SIGINT SIGTERM EXIT

nvmftestfini
