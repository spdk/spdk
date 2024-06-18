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
nqn=nqn.2016-06.io.spdk:cnode1

rpc_py="$rootdir/scripts/rpc.py"

function check_ana_state() {
	local path=$1 ana_state=$2
	# Very rarely a connection is lost and Linux NVMe host tries reconnecting
	# after 10 seconds delay. For this case, set a sufficiently long timeout.
	# Linux NVMe host usually recognizes the new ANA state within 2 seconds.
	local timeout=20
	local ana_state_f=/sys/block/$path/ana_state

	while [[ ! -e $ana_state_f || $(< "$ana_state_f") != "$ana_state" ]] && sleep 1s; do
		if ((timeout-- == 0)); then
			echo "timeout before ANA state ($path) becomes $ana_state"
			return 1
		fi
	done
}

get_subsystem() {
	local nqn=$1 serial=$2 s

	for s in /sys/class/nvme-subsystem/*; do
		[[ $nqn == "$(< "$s/subsysnqn")" && "$serial" == "$(< "$s/serial")" ]] || continue
		echo "${s##*/}" && return 0
	done
	return 1
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
$rpc_py nvmf_create_subsystem "$nqn" -a -s "$NVMF_SERIAL" -r
$rpc_py nvmf_subsystem_add_ns "$nqn" Malloc0
$rpc_py nvmf_subsystem_add_listener "$nqn" -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT
$rpc_py nvmf_subsystem_add_listener "$nqn" -t $TEST_TRANSPORT -a $NVMF_SECOND_TARGET_IP -s $NVMF_PORT

$NVME_CONNECT "${NVME_HOST[@]}" -t $TEST_TRANSPORT -n "$nqn" -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT" -g -G
$NVME_CONNECT "${NVME_HOST[@]}" -t $TEST_TRANSPORT -n "$nqn" -a "$NVMF_SECOND_TARGET_IP" -s "$NVMF_PORT" -g -G
waitforserial "$NVMF_SERIAL"

# Make sure we work with proper subsystem
subsystem=$(get_subsystem "$nqn" "$NVMF_SERIAL")
paths=(/sys/class/nvme-subsystem/$subsystem/nvme*/nvme*c*)
paths=("${paths[@]##*/}")

((${#paths[@]} == 2))

p0=${paths[0]}
p1=${paths[1]}

check_ana_state "$p0" "optimized"
check_ana_state "$p1" "optimized"

# Set IO policy to numa
echo numa > "/sys/class/nvme-subsystem/$subsystem/iopolicy"

$rootdir/scripts/fio-wrapper -p nvmf -i 4096 -d 128 -t randrw -r 6 -v &
fio_pid=$!

sleep 1

$rpc_py nvmf_subsystem_listener_set_ana_state "$nqn" -t $TEST_TRANSPORT -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT" -n inaccessible
$rpc_py nvmf_subsystem_listener_set_ana_state "$nqn" -t $TEST_TRANSPORT -a "$NVMF_SECOND_TARGET_IP" -s "$NVMF_PORT" -n non_optimized

check_ana_state "$p0" "inaccessible"
check_ana_state "$p1" "non-optimized"

$rpc_py nvmf_subsystem_listener_set_ana_state "$nqn" -t $TEST_TRANSPORT -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT" -n non_optimized
$rpc_py nvmf_subsystem_listener_set_ana_state "$nqn" -t $TEST_TRANSPORT -a "$NVMF_SECOND_TARGET_IP" -s "$NVMF_PORT" -n inaccessible

check_ana_state "$p0" "non-optimized"
check_ana_state "$p1" "inaccessible"

wait $fio_pid

$rpc_py nvmf_subsystem_listener_set_ana_state "$nqn" -t $TEST_TRANSPORT -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT" -n optimized
$rpc_py nvmf_subsystem_listener_set_ana_state "$nqn" -t $TEST_TRANSPORT -a "$NVMF_SECOND_TARGET_IP" -s "$NVMF_PORT" -n optimized

check_ana_state "$p0" "optimized"
check_ana_state "$p1" "optimized"

# Set IO policy to round-robin
echo round-robin > "/sys/class/nvme-subsystem/$subsystem/iopolicy"

$rootdir/scripts/fio-wrapper -p nvmf -i 4096 -d 128 -t randrw -r 6 -v &
fio_pid=$!

sleep 1

$rpc_py nvmf_subsystem_listener_set_ana_state "$nqn" -t $TEST_TRANSPORT -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT" -n inaccessible
$rpc_py nvmf_subsystem_listener_set_ana_state "$nqn" -t $TEST_TRANSPORT -a "$NVMF_SECOND_TARGET_IP" -s "$NVMF_PORT" -n non_optimized

check_ana_state "$p0" "inaccessible"
check_ana_state "$p1" "non-optimized"

$rpc_py nvmf_subsystem_listener_set_ana_state "$nqn" -t $TEST_TRANSPORT -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT" -n non_optimized
$rpc_py nvmf_subsystem_listener_set_ana_state "$nqn" -t $TEST_TRANSPORT -a "$NVMF_SECOND_TARGET_IP" -s "$NVMF_PORT" -n inaccessible

check_ana_state "$p0" "non-optimized"
check_ana_state "$p1" "inaccessible"

wait $fio_pid

nvme disconnect -n "$nqn"
waitforserial_disconnect "$NVMF_SERIAL"

$rpc_py nvmf_delete_subsystem "$nqn"

rm -f ./local-job0-0-verify.state
rm -f ./local-job1-1-verify.state

trap - SIGINT SIGTERM EXIT

nvmftestfini
