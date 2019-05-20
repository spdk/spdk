#!/usr/bin/env bash

set -e

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
plugindir=$rootdir/examples/bdev/fio_plugin
rpc_py="$rootdir/scripts/rpc.py"
source "$rootdir/scripts/common.sh"
source "$rootdir/test/common/autotest_common.sh"

function compress_cleanup() {
	timing enter compress_cleanup
	$rootdir/test/app/bdev_svc/bdev_svc &
	bdev_svc_pid=$!
	trap "killprocess $bdev_svc_pid; exit 1" SIGINT SIGTERM EXIT
	waitforlisten $bdev_svc_pid
	bdf=$(iter_pci_class_code 01 08 02 | head -1)
	$rpc_py set_compress_pmd -p 2
	$rpc_py construct_nvme_bdev -b "Nvme0" -t "pcie" -a $bdf
	waitforbdev COMP_$1
	$rpc_py delete_compress_bdev COMP_$1
	$rpc_py destroy_lvol_store -u $2
	trap - SIGINT SIGTERM EXIT
	killprocess $bdev_svc_pid
	rm -rf /tmp/pmem
	timing exit compress_cleanup
}

timing_enter compress_setup
# use the bdev svc to create a compress bdev, this assumes
# there is no other metadata on the nvme device, we will put a
# compress vol on a thin provisioned lvol on nvme
mkdir -p /tmp/pmem
$rootdir/test/app/bdev_svc/bdev_svc &
bdev_svc_pid=$!
trap "killprocess $bdev_svc_pid; compress_cleanup; exit 1" SIGINT SIGTERM EXIT
waitforlisten $bdev_svc_pid
bdf=$(iter_pci_class_code 01 08 02 | head -1)
$rpc_py construct_nvme_bdev -b "Nvme0" -t "pcie" -a $bdf
lvs_u=$($rpc_py construct_lvol_store Nvme0n1 lvs0)
lvb_u=$($rpc_py construct_lvol_bdev -t -u $lvs_u lvb0 5000)
# this will force isal_pmd as some of the CI systems need a qat driver update
$rpc_py set_compress_pmd -p 2
compress_bdev=$($rpc_py construct_compress_bdev -b $lvb_u -p /tmp)
trap - SIGINT SIGTERM EXIT
killprocess $bdev_svc_pid
timing_exit compress_setup

# run bdevio and bdevperf tests
timing_enter compress_test
$rootdir/test/bdev/bdevio/bdevio -w &
bdevio_pid=$!
trap "killprocess $bdevio_pid; compress_cleanup; exit 1" SIGINT SIGTERM EXIT
waitforlisten $bdevio_pid
$rpc_py set_compress_pmd -p 2
$rpc_py construct_nvme_bdev -b "Nvme0" -t "pcie" -a $bdf
waitforbdev $compress_bdev
$rootdir/test/bdev/bdevio/tests.py perform_tests
trap - SIGINT SIGTERM EXIT
killprocess $bdevio_pid

if [ $RUN_NIGHTLY -eq 0 ]; then
	qd=32
	runtime=3
else
	qd=64
	runtime=30
fi

$rootdir/test/bdev/bdevperf/bdevperf -z -q $qd  -o 4096 -w verify -t $runtime &
bdevperf_pid=$!
trap "killprocess $bdevperf_pid; compress_cleanup; exit 1" SIGINT SIGTERM EXIT
waitforlisten $bdevperf_pid
$rpc_py set_compress_pmd -p 2
$rpc_py construct_nvme_bdev -b "Nvme0" -t "pcie" -a $bdf
waitforbdev $compress_bdev
$rootdir/test/bdev/bdevperf/bdevperf.py perform_tests
trap - SIGINT SIGTERM EXIT
killprocess $bdevperf_pid

timing_exit compress_test

# remove the compress on disk metadata and config file
compress_cleanup $lvb_u $lvs_u
timing_exit compress
