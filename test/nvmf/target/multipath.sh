#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=512

rpc_py="$rootdir/scripts/rpc.py"

nvmftestinit

if [ -z $NVMF_SECOND_TARGET_IP ]; then
	echo "only one NIC for nvmf test"
	nvmftestfini
	exit 0
fi

if [ "$TEST_TRANSPORT" != "tcp" ]; then
	echo "run this test only with TCP transport for now"
	nvmftestfini
	exit 0
fi

nvmfappstart -m 0xF

$rpc_py nvmf_create_transport $NVMF_TRANSPORT_OPTS -u 8192

$rpc_py bdev_malloc_create $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE -b Malloc0
$rpc_py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001 -r
$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Malloc0
$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT
$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_SECOND_TARGET_IP -s $NVMF_PORT

nvme connect -t $TEST_TRANSPORT -n "nqn.2016-06.io.spdk:cnode1" -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"
nvme connect -t $TEST_TRANSPORT -n "nqn.2016-06.io.spdk:cnode1" -a "$NVMF_SECOND_TARGET_IP" -s "$NVMF_PORT"

waitforserial "$NVMF_SERIAL"

# We assume only a single subsystem.
subsys_id=$(nvme list-subsys | sed -n 's/nqn.2016-06.io.spdk:cnode1//p' | sed 's/[^0-9]*//g')
ctrl1_id=$(nvme list-subsys | sed -n "s/traddr=$NVMF_FIRST_TARGET_IP trsvcid=$NVMF_PORT//p" | sed 's/[^0-9]*//g')
ctrl2_id=$(nvme list-subsys | sed -n "s/traddr=$NVMF_SECOND_TARGET_IP trsvcid=$NVMF_PORT//p" | sed 's/[^0-9]*//g')

[ $(cat /sys/block/nvme"$subsys_id"c"$ctrl1_id"n1/ana_state) == "optimized" ]
[ $(cat /sys/block/nvme"$subsys_id"c"$ctrl2_id"n1/ana_state) == "optimized" ]

# Set IO policy to numa
echo numa > /sys/class/nvme-subsystem/nvme-subsys$subsys_id/iopolicy

$rootdir/scripts/fio.py -p nvmf -i 4096 -d 128 -t randrw -r 6 -v &
fio_pid=$!

sleep 1

$rpc_py nvmf_subsystem_listener_set_ana_state nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT" -n inaccessible
$rpc_py nvmf_subsystem_listener_set_ana_state nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a "$NVMF_SECOND_TARGET_IP" -s "$NVMF_PORT" -n non_optimized

sleep 1

[ $(cat /sys/block/nvme"$subsys_id"c"$ctrl1_id"n1/ana_state) == "inaccessible" ]
[ $(cat /sys/block/nvme"$subsys_id"c"$ctrl2_id"n1/ana_state) == "non-optimized" ]

$rpc_py nvmf_subsystem_listener_set_ana_state nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT" -n non_optimized
$rpc_py nvmf_subsystem_listener_set_ana_state nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a "$NVMF_SECOND_TARGET_IP" -s "$NVMF_PORT" -n inaccessible

sleep 1

[ $(cat /sys/block/nvme"$subsys_id"c"$ctrl1_id"n1/ana_state) == "non-optimized" ]
[ $(cat /sys/block/nvme"$subsys_id"c"$ctrl2_id"n1/ana_state) == "inaccessible" ]

wait $fio_pid

$rpc_py nvmf_subsystem_listener_set_ana_state nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT" -n optimized
$rpc_py nvmf_subsystem_listener_set_ana_state nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a "$NVMF_SECOND_TARGET_IP" -s "$NVMF_PORT" -n optimized

sleep 1

[ $(cat /sys/block/nvme"$subsys_id"c"$ctrl1_id"n1/ana_state) == "optimized" ]
[ $(cat /sys/block/nvme"$subsys_id"c"$ctrl2_id"n1/ana_state) == "optimized" ]

# Set IO policy to round-robin
echo round-robin > /sys/class/nvme-subsystem/nvme-subsys$subsys_id/iopolicy

$rootdir/scripts/fio.py -p nvmf -i 4096 -d 128 -t randrw -r 6 -v &
fio_pid=$!

sleep 1

$rpc_py nvmf_subsystem_listener_set_ana_state nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT" -n inaccessible
$rpc_py nvmf_subsystem_listener_set_ana_state nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a "$NVMF_SECOND_TARGET_IP" -s "$NVMF_PORT" -n non_optimized

sleep 1

[ $(cat /sys/block/nvme"$subsys_id"c"$ctrl1_id"n1/ana_state) == "inaccessible" ]
[ $(cat /sys/block/nvme"$subsys_id"c"$ctrl2_id"n1/ana_state) == "non-optimized" ]

$rpc_py nvmf_subsystem_listener_set_ana_state nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT" -n non_optimized
$rpc_py nvmf_subsystem_listener_set_ana_state nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a "$NVMF_SECOND_TARGET_IP" -s "$NVMF_PORT" -n inaccessible

sleep 1

[ $(cat /sys/block/nvme"$subsys_id"c"$ctrl1_id"n1/ana_state) == "non-optimized" ]
[ $(cat /sys/block/nvme"$subsys_id"c"$ctrl2_id"n1/ana_state) == "inaccessible" ]

wait $fio_pid

nvme disconnect -n "nqn.2016-06.io.spdk:cnode1" || true

$rpc_py nvmf_delete_subsystem nqn.2016-06.io.spdk:cnode1

rm -f ./local-job0-0-verify.state
rm -f ./local-job1-1-verify.state

trap - SIGINT SIGTERM EXIT

nvmftestfini
