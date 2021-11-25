#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=512

rpc_py="$rootdir/scripts/rpc.py"

function check_ana_state() {
	local subsys_id=$1
	local ctrl_id=$2
	local ana_state=$3
	# Very rarely a connection is lost and Linux NVMe host tries reconnecting
	# after 10 seconds delay. For this case, set a sufficiently long timeout.
	# Linux NVMe host usually recognizes the new ANA state within 2 seconds.
	local timeout=20

	while [ $(cat /sys/block/nvme"$subsys_id"c"$ctrl_id"n1/ana_state) != "$ana_state" ]; do
		sleep 1
		if ((timeout-- == 0)); then
			echo "timeout before ANA state (nvme$subsys_id c$ctrl_id) becomes $ana_state"
			return 1
		fi
	done
}

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

nvme connect -t $TEST_TRANSPORT -n "nqn.2016-06.io.spdk:cnode1" -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT" -g -G
nvme connect -t $TEST_TRANSPORT -n "nqn.2016-06.io.spdk:cnode1" -a "$NVMF_SECOND_TARGET_IP" -s "$NVMF_PORT" -g -G

waitforserial "$NVMF_SERIAL"

# We assume only a single subsystem.
subsys_id=$(nvme list-subsys | sed -n 's/nqn.2016-06.io.spdk:cnode1//p' | sed 's/[^0-9]*//g')
ctrl1_id=$(nvme list-subsys | sed -n "s/traddr=$NVMF_FIRST_TARGET_IP trsvcid=$NVMF_PORT//p" | sed 's/[^0-9]*//g')
ctrl2_id=$(nvme list-subsys | sed -n "s/traddr=$NVMF_SECOND_TARGET_IP trsvcid=$NVMF_PORT//p" | sed 's/[^0-9]*//g')

check_ana_state "$subsys_id" "$ctrl1_id" "optimized"
check_ana_state "$subsys_id" "$ctrl2_id" "optimized"

# Set IO policy to numa
echo numa > /sys/class/nvme-subsystem/nvme-subsys$subsys_id/iopolicy

$rootdir/scripts/fio-wrapper -p nvmf -i 4096 -d 128 -t randrw -r 6 -v &
fio_pid=$!

sleep 1

$rpc_py nvmf_subsystem_listener_set_ana_state nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT" -n inaccessible
$rpc_py nvmf_subsystem_listener_set_ana_state nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a "$NVMF_SECOND_TARGET_IP" -s "$NVMF_PORT" -n non_optimized

check_ana_state "$subsys_id" "$ctrl1_id" "inaccessible"
check_ana_state "$subsys_id" "$ctrl2_id" "non-optimized"

$rpc_py nvmf_subsystem_listener_set_ana_state nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT" -n non_optimized
$rpc_py nvmf_subsystem_listener_set_ana_state nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a "$NVMF_SECOND_TARGET_IP" -s "$NVMF_PORT" -n inaccessible

check_ana_state "$subsys_id" "$ctrl1_id" "non-optimized"
check_ana_state "$subsys_id" "$ctrl2_id" "inaccessible"

wait $fio_pid

$rpc_py nvmf_subsystem_listener_set_ana_state nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT" -n optimized
$rpc_py nvmf_subsystem_listener_set_ana_state nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a "$NVMF_SECOND_TARGET_IP" -s "$NVMF_PORT" -n optimized

check_ana_state "$subsys_id" "$ctrl1_id" "optimized"
check_ana_state "$subsys_id" "$ctrl2_id" "optimized"

# Set IO policy to round-robin
echo round-robin > /sys/class/nvme-subsystem/nvme-subsys$subsys_id/iopolicy

$rootdir/scripts/fio-wrapper -p nvmf -i 4096 -d 128 -t randrw -r 6 -v &
fio_pid=$!

sleep 1

$rpc_py nvmf_subsystem_listener_set_ana_state nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT" -n inaccessible
$rpc_py nvmf_subsystem_listener_set_ana_state nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a "$NVMF_SECOND_TARGET_IP" -s "$NVMF_PORT" -n non_optimized

check_ana_state "$subsys_id" "$ctrl1_id" "inaccessible"
check_ana_state "$subsys_id" "$ctrl2_id" "non-optimized"

$rpc_py nvmf_subsystem_listener_set_ana_state nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT" -n non_optimized
$rpc_py nvmf_subsystem_listener_set_ana_state nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a "$NVMF_SECOND_TARGET_IP" -s "$NVMF_PORT" -n inaccessible

check_ana_state "$subsys_id" "$ctrl1_id" "non-optimized"
check_ana_state "$subsys_id" "$ctrl2_id" "inaccessible"

wait $fio_pid

nvme disconnect -n "nqn.2016-06.io.spdk:cnode1" || true

$rpc_py nvmf_delete_subsystem nqn.2016-06.io.spdk:cnode1

rm -f ./local-job0-0-verify.state
rm -f ./local-job1-1-verify.state

trap - SIGINT SIGTERM EXIT

nvmftestfini
