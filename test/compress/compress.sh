#!/usr/bin/env bash

set -e

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
plugindir=$rootdir/examples/bdev/fio_plugin
rpc_py="$rootdir/scripts/rpc.py"
source "$rootdir/scripts/common.sh"
source "$rootdir/test/common/autotest_common.sh"
source "$rootdir/test/nvmf/common.sh"

function error_cleanup() {
	# force delete pmem file and wipe on-disk metadata
	rm -rf /tmp/pmem
	$rootdir/examples/nvme/perf/perf -q 1 -o 131072 -w write -t 2
}

function destroy_vols() {
	# Gracefully destroy the vols via bdev_compress_delete API.
	# bdev_compress_delete will delete the on-disk metadata as well as
	# the persistent memory file containing its metadata.
	$rpc_py bdev_compress_delete COMP_lvs0/lv0
	$rpc_py bdev_compress_delete COMP_lvs0/lv1
	$rpc_py bdev_lvol_delete_lvstore -l lvs0
}

function create_vols() {
	$rootdir/scripts/gen_nvme.sh --json | $rpc_py load_subsystem_config
	waitforbdev Nvme0n1

	$rpc_py bdev_lvol_create_lvstore Nvme0n1 lvs0
	$rpc_py bdev_lvol_create -t -l lvs0 lv0 100
	waitforbdev lvs0/lv0

	$rpc_py bdev_lvol_create -t -l lvs0 lv1 100
	waitforbdev lvs0/lv1

	# use QAT for lv0, if the test system does not have QAT this will
	# fail which is what we want
	$rpc_py set_compress_pmd -p 1
	$rpc_py bdev_compress_create -b lvs0/lv0 -p /tmp/pmem
	waitforbdev COMP_lvs0/lv0

	# use ISAL for lv1, if ISAL is for some reason not available this will
	# fail which is what we want
	$rpc_py set_compress_pmd -p 2
	$rpc_py bdev_compress_create -b lvs0/lv1 -p /tmp/pmem
	waitforbdev COMP_lvs0/lv1
}

function run_bdevio() {
	$rootdir/test/bdev/bdevio/bdevio -w &
	bdevio_pid=$!
	trap 'killprocess $bdevio_pid; error_cleanup; exit 1' SIGINT SIGTERM EXIT
	waitforlisten $bdevio_pid
	create_vols
	$rootdir/test/bdev/bdevio/tests.py perform_tests
	destroy_vols
	trap - SIGINT SIGTERM EXIT
	killprocess $bdevio_pid
}

function run_bdevperf() {
	$rootdir/test/bdev/bdevperf/bdevperf -z -q $1 -o $2 -w verify -t $3 &
	bdevperf_pid=$!
	trap 'killprocess $bdevperf_pid; error_cleanup; exit 1' SIGINT SIGTERM EXIT
	waitforlisten $bdevperf_pid
	create_vols
	$rootdir/test/bdev/bdevperf/bdevperf.py perform_tests
	destroy_vols
	trap - SIGINT SIGTERM EXIT
	killprocess $bdevperf_pid
}

mkdir -p /tmp/pmem

# per patch bdevperf uses slightly different params than nightly
run_bdevperf 32 4096 3

if [ $RUN_NIGHTLY -eq 1 ]; then
	run_bdevio
	run_bdevperf 64 16384 30

	# run perf on nvmf target w/compressed vols
	export TEST_TRANSPORT=tcp && nvmftestinit
	nvmfappstart "-m 0x7"
	trap "nvmftestfini; error_cleanup; exit 1" SIGINT SIGTERM EXIT

	# Create an NVMe-oF subsystem and add compress bdevs as namespaces
	$rpc_py nvmf_create_transport -t $TEST_TRANSPORT -u 8192
	create_vols
	$rpc_py nvmf_subsystem_create nqn.2016-06.io.spdk:cnode0 -a -s SPDK0
	$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode0 COMP_lvs0/lv0
	$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode0 COMP_lvs0/lv1
	$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode0 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT

	# Start random read writes in the background
	$rootdir/examples/nvme/perf/perf -r "trtype:$TEST_TRANSPORT adrfam:IPv4 traddr:$NVMF_FIRST_TARGET_IP trsvcid:$NVMF_PORT" -o 4096 -q 64 -s 512 -w randrw -t 30 -c 0x18 -M 50 &
	perf_pid=$!

	# Wait for I/O to complete
	trap 'killprocess $perf_pid; compress_err_cleanup; exit 1' SIGINT SIGTERM EXIT
	wait $perf_pid
	destroy_vols

	trap - SIGINT SIGTERM EXIT
	nvmftestfini
fi

rm -rf /tmp/pmem
