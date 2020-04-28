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

trap 'process_shm --id $NVMF_APP_SHM_ID; killprocess $nvmfpid; nvmftestfini $1; exit 1' SIGINT SIGTERM EXIT

$rpc_py bdev_malloc_create $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE -b Malloc0

# We cannot configure the bdev with an incredibly high latency up front because connect will not work properly.
$rpc_py bdev_delay_create -b Malloc0 -d Delay0 -r 30 -t 30 -w 30 -n 30

$rpc_py nvmf_create_transport $NVMF_TRANSPORT_OPTS -u 8192
$rpc_py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a -s $NVMF_SERIAL
$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Delay0
$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT

nvme connect -t $TEST_TRANSPORT -n "nqn.2016-06.io.spdk:cnode1" -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"

waitforserial "$NVMF_SERIAL"

# Once our timed out I/O complete, we will still have 10 sec of I/O.
$rootdir/scripts/fio.py -p nvmf -i 4096 -d 1 -t write -r 60 -v &
fio_pid=$!

sleep 3

# The kernel initiator has a default timeout of 30 seconds. delay for 31 to trigger initiator reconnect.
$rpc_py bdev_delay_update_latency Delay0 avg_read 31000000
$rpc_py bdev_delay_update_latency Delay0 avg_write 31000000
$rpc_py bdev_delay_update_latency Delay0 p99_read 31000000
$rpc_py bdev_delay_update_latency Delay0 p99_write 310000000

sleep 3

# Reset these values so that subsequent I/O will complete in a timely manner.
$rpc_py bdev_delay_update_latency Delay0 avg_read 30
$rpc_py bdev_delay_update_latency Delay0 avg_write 30
$rpc_py bdev_delay_update_latency Delay0 p99_read 30
$rpc_py bdev_delay_update_latency Delay0 p99_write 30

fio_status=0
wait $fio_pid || fio_status=$?

nvme disconnect -n "nqn.2016-06.io.spdk:cnode1" || true

if [ $fio_status -eq 0 ]; then
	echo "nvmf hotplug test: fio successful as expected"
else
	echo "nvmf hotplug test: fio failed, expected success"
	nvmftestfini
	exit 1
fi

$rpc_py nvmf_delete_subsystem nqn.2016-06.io.spdk:cnode1

rm -f ./local-job0-0-verify.state

trap - SIGINT SIGTERM EXIT

nvmftestfini
