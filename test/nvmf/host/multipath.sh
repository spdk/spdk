#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2020 Intel Corporation
#  All rights reserved.
#
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=512

rpc_py="$rootdir/scripts/rpc.py"
bpf_sh="$rootdir/scripts/bpftrace.sh"

bdevperf_rpc_sock=/var/tmp/bdevperf.sock

# NQN prefix to use for subsystem NQNs
NQN=nqn.2016-06.io.spdk:cnode1

nvmftestinit

nvmfappstart -m 0x3
nvmfapp_pid=$!

$rpc_py nvmf_create_transport $NVMF_TRANSPORT_OPTS -u 8192
$rpc_py bdev_malloc_create $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE -b Malloc0
# Set -r to enable ANA reporting feature
$rpc_py nvmf_create_subsystem $NQN -a -s SPDK00000000000001 -r -m 2
$rpc_py nvmf_subsystem_add_ns $NQN Malloc0
$rpc_py nvmf_subsystem_add_listener $NQN -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT
$rpc_py nvmf_subsystem_add_listener $NQN -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_SECOND_PORT

$rootdir/build/examples/bdevperf -m 0x4 -z -r $bdevperf_rpc_sock -q 128 -o 4096 -w verify -t 90 &> $testdir/try.txt &
bdevperf_pid=$!

trap 'process_shm --id $NVMF_APP_SHM_ID; rm -f $testdir/try.txt; killprocess $bdevperf_pid; nvmftestfini; exit 1' SIGINT SIGTERM EXIT
waitforlisten $bdevperf_pid $bdevperf_rpc_sock

# Create a controller and set multipath behavior
# bdev_retry_count is set to -1 means infinite reconnects
$rpc_py -s $bdevperf_rpc_sock bdev_nvme_set_options -r -1
# -l -1 ctrlr_loss_timeout_sec -1 means infinite reconnects
# -o 10 reconnect_delay_sec time to delay a reconnect retry is limited to 10 sec
$rpc_py -s $bdevperf_rpc_sock bdev_nvme_attach_controller -b Nvme0 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT -f ipv4 -n $NQN -l -1 -o 10
$rpc_py -s $bdevperf_rpc_sock bdev_nvme_attach_controller -b Nvme0 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_SECOND_PORT -f ipv4 -n $NQN -x multipath -l -1 -o 10

function set_ANA_state() {
	$rpc_py nvmf_subsystem_listener_set_ana_state $NQN -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT -n $1
	$rpc_py nvmf_subsystem_listener_set_ana_state $NQN -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_SECOND_PORT -n $2
}

# check for io on the expected ANA state port
function confirm_io_on_port() {
	$bpf_sh $nvmfapp_pid $rootdir/scripts/bpf/nvmf_path.bt &> $testdir/trace.txt &
	dtrace_pid=$!
	sleep 6
	active_port=$($rpc_py nvmf_subsystem_get_listeners $NQN | jq -r '.[] | select (.ana_states[0].ana_state=="'$1'") | .address.trsvcid')
	cat $testdir/trace.txt
	port=$(cut < $testdir/trace.txt -d ']' -f1 | awk '$1=="@path['$NVMF_FIRST_TARGET_IP'," {print $2}' | sed -n '1p')
	[[ "$active_port" == "$port" ]]
	[[ "$port" == "$2" ]]
	kill $dtrace_pid
	rm -f $testdir/trace.txt
}

$rootdir/examples/bdev/bdevperf/bdevperf.py -t 120 -s $bdevperf_rpc_sock perform_tests &
rpc_pid=$!

sleep 1

# Set ANA state to each listener
set_ANA_state non_optimized optimized
# Check the IO on expected port with ANA state set
confirm_io_on_port "optimized" $NVMF_SECOND_PORT

# Check traffic paths with different ANA states
set_ANA_state non_optimized inaccessible
confirm_io_on_port "non_optimized" $NVMF_PORT

set_ANA_state inaccessible optimized
confirm_io_on_port "optimized" $NVMF_SECOND_PORT

# Not expecting the io on any port
set_ANA_state inaccessible inaccessible
confirm_io_on_port "" ""

set_ANA_state non_optimized optimized
confirm_io_on_port "optimized" $NVMF_SECOND_PORT

# Remove listener to monitor multipath function
$rpc_py nvmf_subsystem_remove_listener $NQN -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_SECOND_PORT
sleep 1

# Expect IO on alternate path
confirm_io_on_port "non_optimized" $NVMF_PORT

# Add listener back with optimized state, traffic should switch to optimized port
$rpc_py nvmf_subsystem_add_listener $NQN -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_SECOND_PORT
$rpc_py nvmf_subsystem_listener_set_ana_state $NQN -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_SECOND_PORT -n optimized

# wait for the io to switch to second port
sleep 6
confirm_io_on_port "optimized" $NVMF_SECOND_PORT

wait $rpc_pid
cat $testdir/try.txt

killprocess $bdevperf_pid

$rpc_py nvmf_delete_subsystem $NQN

trap - SIGINT SIGTERM EXIT

rm -f $testdir/try.txt
nvmftestfini
