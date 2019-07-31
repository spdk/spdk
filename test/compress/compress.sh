#!/usr/bin/env bash

set -e

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
plugindir=$rootdir/examples/bdev/fio_plugin
rpc_py="$rootdir/scripts/rpc.py"
source "$rootdir/scripts/common.sh"
source "$rootdir/test/common/autotest_common.sh"
source "$rootdir/test/nvmf/common.sh"

function compress_err_cleanup() {
	rm -rf /tmp/pmem
	$rootdir/examples/nvme/perf/perf -q 1 -o 131072 -w write -t 2
}

function run_bdevio() {
	$rootdir/test/bdev/bdevio/bdevio -w &
	bdevio_pid=$!
	trap "killprocess $bdevio_pid; compress_err_cleanup; exit 1" SIGINT SIGTERM EXIT
	waitforlisten $bdevio_pid
	$rpc_py set_compress_pmd -p $1
	$rpc_py construct_nvme_bdev -b "Nvme0" -t "pcie" -a $bdf
	waitforbdev $compress_bdev
	$rootdir/test/bdev/bdevio/tests.py perform_tests
	trap - SIGINT SIGTERM EXIT
	killprocess $bdevio_pid
}

function run_bdevperf() {
	$rootdir/test/bdev/bdevperf/bdevperf -z -q $qd  -o $iosize -w verify -t $runtime &
	bdevperf_pid=$!
	trap "killprocess $bdevperf_pid; compress_err_cleanup; exit 1" SIGINT SIGTERM EXIT
	waitforlisten $bdevperf_pid
	$rpc_py set_compress_pmd -p $1
	$rpc_py construct_nvme_bdev -b "Nvme0" -t "pcie" -a $bdf
	waitforbdev $compress_bdev
	$rootdir/test/bdev/bdevperf/bdevperf.py perform_tests
	if [ $RUN_NIGHTLY -eq 0 ]; then
		# now cleanup the vols, deleting the compression vol also deletes the pmem file
		$rpc_py delete_compress_bdev COMP_lvs0/lv0
		$rpc_py destroy_lvol_store -l lvs0
	fi
	trap - SIGINT SIGTERM EXIT
	killprocess $bdevperf_pid
}

# use the bdev svc to create a compress bdev, this assumes
# there is no other metadata on the nvme device, we will put a
# compress vol on a thin provisioned lvol on nvme
mkdir -p /tmp/pmem
$rootdir/test/app/bdev_svc/bdev_svc &
bdev_svc_pid=$!
trap "killprocess $bdev_svc_pid; compress_err_cleanup; exit 1" SIGINT SIGTERM EXIT
waitforlisten $bdev_svc_pid
bdf=$(iter_pci_class_code 01 08 02 | head -1)
$rpc_py construct_nvme_bdev -b "Nvme0" -t "pcie" -a $bdf
lvs_u=$($rpc_py construct_lvol_store Nvme0n1 lvs0)
$rpc_py construct_lvol_bdev -t -u $lvs_u lv0 100
compress_bdev=$($rpc_py construct_compress_bdev -b lvs0/lv0 -p /tmp)
trap - SIGINT SIGTERM EXIT
killprocess $bdev_svc_pid

timing_enter compress_test

# run bdevio test, ISAL for per patch and both for nightly.
run_bdevio(2)
if [ $RUN_NIGHTLY -eq 1 ]; then
	run_bdevio(1)
fi

# run bdevperf with slightly different params for nightly
qd=32
runtime=3
iosize=4096
run_bdevperf(2)
if [ $RUN_NIGHTLY -eq 1 ]; then
	qd=64
	runtime=30
	iosize=16384
	run_bdevperf(1)
fi

# run perf with nvmf using compress bdev for nightly test only. To save time,
# this test will alwways run on QAT if available and ISAL if not but we will
# not run it twice.
if [ $RUN_NIGHTLY -eq 1 ]; then
	export TEST_TRANSPORT=tcp && nvmftestinit
	nvmfappstart "-m 0x7"
	trap "nvmftestfini; compress_err_cleanup; exit 1" SIGINT SIGTERM EXIT

	# Create an NVMe-oF subsystem and add compress bdev as a namespace
	$rpc_py nvmf_create_transport -t $TEST_TRANSPORT -u 8192
	$rpc_py construct_nvme_bdev -b "Nvme0" -t "pcie" -a $bdf
	waitforbdev $compress_bdev
	$rpc_py nvmf_subsystem_create nqn.2016-06.io.spdk:cnode0 -a -s SPDK0
	$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode0 $compress_bdev
	$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode0 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT

	# Start random read writes in the background
	$rootdir/examples/nvme/perf/perf -r "trtype:$TEST_TRANSPORT adrfam:IPv4 traddr:$NVMF_FIRST_TARGET_IP trsvcid:$NVMF_PORT" -o 4096 -q $qd -s 512 -w randrw -t $runtime -c 0x18 -M 50 &
	perf_pid=$!

	# Wait for I/O to complete
	trap "killprocess $perf_pid; compress_err_cleanup; exit 1" SIGINT SIGTERM EXIT
	wait $perf_pid

	# now cleanup the vols, deleting the compression vol also deletes the pmem file
	$rpc_py delete_compress_bdev COMP_lvs0/lv0
	$rpc_py destroy_lvol_store -l lvs0

	trap - SIGINT SIGTERM EXIT
	nvmftestfini
fi

timing_exit compress_test
