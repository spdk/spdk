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
	# force delete pmem file and wipe metadata
	rm -rf /tmp/pmem
	$rootdir/examples/nvme/perf/perf -q 1 -o 131072 -w write -t 2
}

function destroy_vols() {
	# gracefully destroy the vols via API, deleting the compression vol also
	# deletes the pmem file
	$rpc_py delete_compress_bdev COMP_lvs0/lv0
	$rpc_py delete_compress_bdev COMP_lvs0/lv1
	$rpc_py destroy_lvol_store -l lvs0
}

function create_vols() {
	$rpc_py construct_nvme_bdev -b "Nvme0" -t "pcie" -a $bdf
	lvs_u=$($rpc_py construct_lvol_store Nvme0n1 lvs0)
	$rpc_py construct_lvol_bdev -t -u $lvs_u lv0 100
	$rpc_py construct_lvol_bdev -t -u $lvs_u lv1 100
	# use QAT for lv0, if the test system does not have QAT this will
	# fail which is what we want
	$rpc_py set_compress_pmd -p 1
	$rpc_py construct_compress_bdev -b lvs0/lv0 -p /tmp/pmem
	# use ISAL for lv1, if ISAL is for some reason not available this will
	# fail which is what we want
	$rpc_py set_compress_pmd -p 2
	compress_bdev=$($rpc_py construct_compress_bdev -b lvs0/lv1 -p /tmp/pmem)
	waitforbdev $compress_bdev
}

function run_bdevperf() {
	$rootdir/test/bdev/bdevperf/bdevperf -z -q $1 -o $2 -w verify -t $3 &
	bdevperf_pid=$!
	trap "killprocess $bdevperf_pid; error_cleanup; exit 1" SIGINT SIGTERM EXIT
	waitforlisten $bdevperf_pid
	create_vols
	$rootdir/test/bdev/bdevperf/bdevperf.py perform_tests
	destroy_vols
	trap - SIGINT SIGTERM EXIT
	killprocess $bdevperf_pid
}

timing_enter compress_test
mkdir -p /tmp/pmem
bdf=$(iter_pci_class_code 01 08 02 | head -1)

# run bdevperf
run_bdevperf 32 4096 3

if [ $RUN_NIGHTLY -eq 1 ]; then
	# run bdevio test
	$rootdir/test/bdev/bdevio/bdevio -w &
	bdevio_pid=$!
	trap "killprocess $bdevio_pid; error_cleanup; exit 1" SIGINT SIGTERM EXIT
	waitforlisten $bdevio_pid
	create_vols
	$rootdir/test/bdev/bdevio/tests.py perform_tests
	destroy_vols
	trap - SIGINT SIGTERM EXIT
	killprocess $bdevio_pid

	# run bdevperf
	run_bdevperf 64 16384 30

	# run perf on nvmf target w/compressed vols created w/prev test
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
	trap "killprocess $perf_pid; compress_err_cleanup; exit 1" SIGINT SIGTERM EXIT
	wait $perf_pid
	destroy_vols
	rm -rf /tmp/pmem

	trap - SIGINT SIGTERM EXIT
	nvmftestfini
fi

timing_exit compress_test
