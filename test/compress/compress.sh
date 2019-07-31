#!/usr/bin/env bash

set -e

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
plugindir=$rootdir/examples/bdev/fio_plugin
rpc_py="$rootdir/scripts/rpc.py"
source "$rootdir/scripts/common.sh"
source "$rootdir/test/common/autotest_common.sh"
source "$rootdir/test/nvmf/common.sh"

function wipe_disk() {
	# force delete pmem file and wipe metadata
	rm -rf /tmp/pmem
	$rootdir/examples/nvme/perf/perf -q 1 -o 131072 -w write -t 2
}

function destroy_vols() {
	# gracefully cleanup the vols, deleting the compression vol also
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
	compress_bdev1=$($rpc_py construct_compress_bdev -b lvs0/lv0 -p /tmp)
	# use ISAL for lv1, if ISAL is for some reason not available this will
	# fail which is what we wan
	$rpc_py set_compress_pmd -p 2
	compress_bdev2=$($rpc_py construct_compress_bdev -b lvs0/lv1 -p /tmp)
	waitforbdev $compress_bdev2
}

mkdir -p /tmp/pmem
bdf=$(iter_pci_class_code 01 08 02 | head -1)

# run bdevio test
timing_enter compress_test
$rootdir/test/bdev/bdevio/bdevio -w &
bdevio_pid=$!
trap "killprocess $bdevio_pid; wipe_disk; exit 1" SIGINT SIGTERM EXIT
waitforlisten $bdevio_pid
create_vols
$rootdir/test/bdev/bdevio/tests.py perform_tests
trap - SIGINT SIGTERM EXIT
killprocess $bdevio_pid
wipe_disk

#run bdevperf with slightly different params for nightly
qd=32
runtime=3
iosize=4096
if [ $RUN_NIGHTLY -eq 1 ]; then
	qd=64
	runtime=30
	iosize=16384
fi
$rootdir/test/bdev/bdevperf/bdevperf -z -q $qd  -o $iosize -w verify -t $runtime &
bdevperf_pid=$!
trap "killprocess $bdevperf_pid; wipe_disk; exit 1" SIGINT SIGTERM EXIT
waitforlisten $bdevperf_pid
create_vols
$rootdir/test/bdev/bdevperf/bdevperf.py perform_tests
if [ $RUN_NIGHTLY -eq 0 ]; then
	destroy_vols
fi
trap - SIGINT SIGTERM EXIT
killprocess $bdevperf_pid

# run perf with nvmf using compress bdev for nightly test only.
if [ $RUN_NIGHTLY -eq 1 ]; then
	export TEST_TRANSPORT=tcp && nvmftestinit
	nvmfappstart "-m 0x7"
	trap "nvmftestfini; wipe_disk; exit 1" SIGINT SIGTERM EXIT

	# Create an NVMe-oF subsystem and add compress bdev as a namespace
	$rpc_py nvmf_create_transport -t $TEST_TRANSPORT -u 8192
	create_vols
	$rpc_py nvmf_subsystem_create nqn.2016-06.io.spdk:cnode0 -a -s SPDK0
	$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode0 COMP_lvs0/lv0
	$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode0 COMP_lvs0/lv1
	$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode0 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT

	# Start random read writes in the background
	$rootdir/examples/nvme/perf/perf -r "trtype:$TEST_TRANSPORT adrfam:IPv4 traddr:$NVMF_FIRST_TARGET_IP trsvcid:$NVMF_PORT" -o 4096 -q $qd -s 512 -w randrw -t $runtime -c 0x18 -M 50 &
	perf_pid=$!

	# Wait for I/O to complete
	trap "killprocess $perf_pid; wipe_disk; exit 1" SIGINT SIGTERM EXIT
	wait $perf_pid
	destroy_vols

	trap - SIGINT SIGTERM EXIT
	nvmftestfini
fi

timing_exit compress_test
