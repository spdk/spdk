#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2020 Intel Corporation
#  Copyright (C) 2025 Dell Inc, or its subsidiaries
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
	nvmfappstart -m 0xE

	$rpc_py nvmf_create_transport $NVMF_TRANSPORT_OPTS -u 8192

	timing_enter create_subsystem
	# Create subsystem
	rm -rf $testdir/rpcs.txt
	cat <<- EOL >> $testdir/rpcs.txt
		bdev_malloc_create $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE -b Malloc0
		nvmf_create_subsystem nqn.2016-06.io.spdk:cnode0 -s SPDK0
	EOL
	if [ "$1" = "delay" ]; then
		cat <<- EOL >> $testdir/rpcs.txt
			bdev_delay_create -b Malloc0 -d Delay0 -r 3000000 -t 3000000 -w 3000000 -n 3000000
			nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode0 Delay0
		EOL
	else
		cat <<- EOL >> $testdir/rpcs.txt
			nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode0 Malloc0
		EOL
	fi

	cat <<- EOL >> $testdir/rpcs.txt
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

	killprocess $nvmfpid
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

	# $3 = timeout in 0.25s
	if [ -z "$3" ]; then
		exit 1
	fi

	local ret=1
	local i
	for ((i = $3; i != 0; i--)); do
		read_io_count=$($rpc_py -s $1 bdev_get_iostat --names $2 | jq -r '.bdevs[0].num_read_ops')
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

function run_bdev_perf() {
	# $1 = run time
	if [ -z "$1" ]; then
		exit 1
	fi

	# Run bdevperf
	run_app_bg "$SPDK_EXAMPLE_DIR/bdevperf" -r /var/tmp/bdevperf.sock --json <(gen_nvmf_target_json "0") -q 64 -o 65536 -w verify -t $1
	perfpid=$!
	waitforlisten $perfpid /var/tmp/bdevperf.sock
	$rpc_py -s /var/tmp/bdevperf.sock framework_wait_init

	# Expand the trap to clean up bdevperf if something goes wrong
	trap 'process_shm --id $NVMF_APP_SHM_ID; kill -9 $perfpid || true; nvmftestfini; exit 1' SIGINT SIGTERM EXIT
}

function clean_bdev_perf() {
	# TODO: Right now the NVMe-oF initiator will not correctly detect broken connections
	# and so it will never shut down. Just kill it.
	kill -9 $perfpid || true

	# Since we abruptly terminate $perfpid above, we need to do some cleanup on our own.
	# In particular, we need to get rid of the cpu lock files that may potentially prevent
	# the next instance of bdevperf from running.
	# FIXME: Can't we just SIGTERM $perfpid above?
	rm -f /var/tmp/spdk_cpu_lock*
}

# Add a host, start I/O, remove host, re-add host
function nvmf_host_management_tc1() {
	starttarget
	run_bdev_perf 10
	waitforio /var/tmp/bdevperf.sock Nvme0n1 10

	# Remove the host while bdevperf is still running, then re-add it quickly. The host
	# may attempt to reconnect.
	$rpc_py nvmf_subsystem_remove_host nqn.2016-06.io.spdk:cnode0 nqn.2016-06.io.spdk:host0
	$rpc_py nvmf_subsystem_add_host nqn.2016-06.io.spdk:cnode0 nqn.2016-06.io.spdk:host0

	sleep 1
	clean_bdev_perf

	# Run bdevperf
	run_app "$SPDK_EXAMPLE_DIR/bdevperf" --json <(gen_nvmf_target_json "0") -q 64 -o 65536 -w verify -t 1
	stoptarget
}

function run_io_and_calculate_remove_time() {
	starttarget delay
	run_bdev_perf 30
	# Wait 20s for io - waitforio is taking time in 0.25s units
	waitforio /var/tmp/bdevperf.sock Nvme0n1 80

	start_time=$(date +%s)
	expected_status=$1
	shift
	if [ "$expected_status" = "fail" ]; then
		NOT $rpc_py nvmf_subsystem_remove_host nqn.2016-06.io.spdk:cnode0 nqn.2016-06.io.spdk:host0 "$@"
	else
		$rpc_py nvmf_subsystem_remove_host nqn.2016-06.io.spdk:cnode0 nqn.2016-06.io.spdk:host0 "$@"
	fi
	end_time=$(date +%s)
	remove_host_time=$((end_time - start_time))

	sleep 1
	clean_bdev_perf
	stoptarget
}

# Add a host on delay device, start I/O, remove host and check the time
function nvmf_host_management_tc2() {
	run_io_and_calculate_remove_time success

	echo "Remove host time was $remove_host_time"
	# With delay device set to 3s remove host should take at least 2 seconds (timestamp resolution is in seconds, so not always >3s)
	if [ "$remove_host_time" -lt 2 ]; then
		false
	fi
}

# Add a host on delay device, start I/O, remove host with tight timeout and check the time
function nvmf_host_management_tc3() {
	# Remove host with timeout = 1000ms, expected fail
	run_io_and_calculate_remove_time fail --timeout-ms 1000

	echo "Remove host time was $remove_host_time"
	# Timeout is set to 1s so we shouldn't need more than 2s
	if [ "$remove_host_time" -gt 2 ]; then
		false
	fi
}

#Init nvmf_tgt
nvmftestinit

nvmf_host_management_tc1

nvmf_host_management_tc2

nvmf_host_management_tc3

nvmftestfini

trap - SIGINT SIGTERM EXIT
