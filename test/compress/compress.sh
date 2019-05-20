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
	rm -rf $testdir/compress.conf
	$rootdir/test/app/bdev_svc/bdev_svc &
	bdev_svc_pid=$!
	trap "killprocess $bdev_svc_pid; exit 1" SIGINT SIGTERM EXIT
	waitforlisten $bdev_svc_pid
	bdf=$(iter_pci_class_code 01 08 02 | head -1)
	$rpc_py construct_nvme_bdev -b "Nvme0" -t "pcie" -a $bdf
	waitforbdev COMP_$1
	$rpc_py delete_compress_bdev COMP_$1
	$rpc_py destroy_lvol_store -u $2
	trap - SIGINT SIGTERM EXIT
	killprocess $bdev_svc_pid
	timing exit compress_cleanup
}


timing_enter compress_setup
# create a one entry conf file for the nvme device
$rootdir/scripts/gen_nvme.sh >> $testdir/compress.conf

# use the bdev svc to create a compress bdev, this assumes
# there is no other metadata on the nvme device, we will put a
# compress vol on a thin provisioned lvol on nvme
$rootdir/test/app/bdev_svc/bdev_svc &
bdev_svc_pid=$!
trap "killprocess $bdev_svc_pid; compress_cleanup; exit 1" SIGINT SIGTERM EXIT
waitforlisten $bdev_svc_pid
bdf=$(iter_pci_class_code 01 08 02 | head -1)
$rpc_py construct_nvme_bdev -b "Nvme0" -t "pcie" -a $bdf
lvs_u=$($rpc_py construct_lvol_store Nvme0n1 lvs0)
lvb_u=$($rpc_py construct_lvol_bdev -t -u $lvs_u lvb0 10000)
$rpc_py construct_compress_bdev -b $lvb_u -p /tmp
trap - SIGINT SIGTERM EXIT
killprocess $bdev_svc_pid
timing_exit compress_setup

# run bdevio and bdevperf tests
timing_enter compress_test
$rootdir/test/bdev/bdevio/bdevio -c $testdir/compress.conf
if [ $RUN_NIGHTLY -eq 0 ]; then
	$rootdir/test/bdev/bdevperf/bdevperf -c $testdir/compress.conf -q 32 -o 4096 -w verify -t 3
else
	$rootdir/test/bdev/bdevperf/bdevperf -c $testdir/compress.conf -q 64 -o 4096 -w verify -t 30
fi
timing_exit compress_test

# remove the compress on disk metadata and config file
compress_cleanup $lvb_u $lvs_u
timing_exit compress
