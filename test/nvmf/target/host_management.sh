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

function starttarget() {
	# Start the target
	nvmfappstart -m 0x1E

	$rpc_py nvmf_create_transport $NVMF_TRANSPORT_OPTS -u 8192

	timing_enter create_subsystem
	# Create subsystem
	rm -rf $testdir/rpcs.txt
	cat <<- EOL >> $testdir/rpcs.txt
		bdev_malloc_create $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE -b Malloc0
		nvmf_create_subsystem nqn.2016-06.io.spdk:cnode0 -s SPDK0
		nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode0 Malloc0
		nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode0 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT
		nvmf_subsystem_add_host nqn.2016-06.io.spdk:cnode0 nqn.2016-06.io.spdk:host0
	EOL
	$rpc_py < $testdir/rpcs.txt
	timing_exit create_subsystems

}

function stoptarget() {
	rm -f ./local-job0-0-verify.state
	rm -rf $testdir/bdevperf.conf
	rm -rf $testdir/rpcs.txt

	nvmftestfini
}

function waitforio() {
	# $1 = RPC socket
	if [ -z "$1" ]; then
		exit 1
	fi
	# $2 = bdev name
	if [ -z "$2" ]; then
		exit 1
	fi
	local ret=1
	local i
	for ((i = 10; i != 0; i--)); do
		read_io_count=$($rpc_py -s $1 bdev_get_iostat -b $2 | jq -r '.bdevs[0].num_read_ops')
		# A few I/O will happen during initial examine.  So wait until at least 100 I/O
		#  have completed to know that bdevperf is really generating the I/O.
		if [ $read_io_count -ge 100 ]; then
			ret=0
			break
		fi
		sleep 0.25
	done
	return $ret
}

# Add a host, start I/O, remove host, re-add host
function nvmf_host_management() {
	starttarget

	# Run bdevperf
	$rootdir/build/examples/bdevperf -r /var/tmp/bdevperf.sock --json <(gen_nvmf_target_json "0") -q 64 -o 65536 -w verify -t 10 &
	perfpid=$!
	waitforlisten $perfpid /var/tmp/bdevperf.sock
	$rpc_py -s /var/tmp/bdevperf.sock framework_wait_init

	# Expand the trap to clean up bdevperf if something goes wrong
	trap 'process_shm --id $NVMF_APP_SHM_ID; kill -9 $perfpid || true; nvmftestfini; exit 1' SIGINT SIGTERM EXIT

	waitforio /var/tmp/bdevperf.sock Nvme0n1

	# Remove the host while bdevperf is still running, then re-add it quickly. The host
	# may attempt to reconnect.
	$rpc_py nvmf_subsystem_remove_host nqn.2016-06.io.spdk:cnode0 nqn.2016-06.io.spdk:host0
	$rpc_py nvmf_subsystem_add_host nqn.2016-06.io.spdk:cnode0 nqn.2016-06.io.spdk:host0

	sleep 1

	# TODO: Right now the NVMe-oF initiator will not correctly detect broken connections
	# and so it will never shut down. Just kill it.
	kill -9 $perfpid || true

	# Since we abruptly terminate $perfpid above, we need to do some cleanup on our own.
	# In particular, we need to get rid of the cpu lock files that may potentially prevent
	# the next instance of bdevperf from running.
	# FIXME: Can't we just SIGTERM $perfpid above?
	rm -f /var/tmp/spdk_cpu_lock*

	# Run bdevperf
	$rootdir/build/examples/bdevperf -r /var/tmp/bdevperf.sock --json <(gen_nvmf_target_json "0") -q 64 -o 65536 -w verify -t 1

	stoptarget
}

nvmftestinit

run_test "nvmf_host_management" nvmf_host_management

trap - SIGINT SIGTERM EXIT
