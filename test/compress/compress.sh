#!/usr/bin/env bash

set -e

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
plugindir=$rootdir/examples/bdev/fio_plugin
rpc_py="$rootdir/scripts/rpc.py"
source "$rootdir/scripts/common.sh"
source "$rootdir/test/common/autotest_common.sh"

function compress_err_cleanup() {
	rm -rf /tmp/pmem
	$rootdir/examples/nvme/perf/perf -q 1 -o 131072 -w write -t 2
}

# use the bdev svc to create a compress bdev, this assumes
# there is no other metadata on the nvme device, we will put a
# compress vol on a thin provisioned lvol on nvme
mkdir -p /tmp/pmem
$rootdir/test/app/bdev_svc/bdev_svc &
bdev_svc_pid=$!
trap 'killprocess $bdev_svc_pid; compress_err_cleanup; exit 1' SIGINT SIGTERM EXIT
waitforlisten $bdev_svc_pid
bdf=$(iter_pci_class_code 01 08 02 | head -1)
$rpc_py bdev_nvme_attach_controller -b "Nvme0" -t "pcie" -a $bdf
lvs_u=$($rpc_py bdev_lvol_create_lvstore Nvme0n1 lvs0)
$rpc_py bdev_lvol_create -t -u $lvs_u lv0 100
# this will force isal_pmd as some of the CI systems need a qat driver update
$rpc_py set_compress_pmd -p 2
compress_bdev=$($rpc_py bdev_compress_create -b lvs0/lv0 -p /tmp)
trap - SIGINT SIGTERM EXIT
killprocess $bdev_svc_pid

# run bdevio test
timing_enter compress_test
$rootdir/test/bdev/bdevio/bdevio -w &
bdevio_pid=$!
trap 'killprocess $bdevio_pid; compress_err_cleanup; exit 1' SIGINT SIGTERM EXIT
waitforlisten $bdevio_pid
$rpc_py set_compress_pmd -p 2
$rpc_py bdev_nvme_attach_controller -b "Nvme0" -t "pcie" -a $bdf
waitforbdev $compress_bdev
$rootdir/test/bdev/bdevio/tests.py perform_tests
trap - SIGINT SIGTERM EXIT
killprocess $bdevio_pid

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
trap 'killprocess $bdevperf_pid; compress_err_cleanup; exit 1' SIGINT SIGTERM EXIT
waitforlisten $bdevperf_pid
$rpc_py set_compress_pmd -p 2
$rpc_py bdev_nvme_attach_controller -b "Nvme0" -t "pcie" -a $bdf
waitforbdev $compress_bdev
$rootdir/test/bdev/bdevperf/bdevperf.py perform_tests

# now cleanup the vols, deleting the compression vol also deletes the pmem file
$rpc_py bdev_compress_delete COMP_lvs0/lv0
$rpc_py bdev_lvol_delete_lvstore -l lvs0

trap - SIGINT SIGTERM EXIT
killprocess $bdevperf_pid
timing_exit compress_test
