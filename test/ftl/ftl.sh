#!/usr/bin/env bash

set -e

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
rpc_py=$rootdir/scripts/rpc.py

source $rootdir/test/common/autotest_common.sh

function at_ftl_exit() {
	# restore original driver
	PCI_WHITELIST="$device" PCI_BLACKLIST="" DRIVER_OVERRIDE="$ocssd_original_dirver" ./scripts/setup.sh
}

read device _ <<< "$OCSSD_PCI_DEVICES"

if [[ -z "$device" ]]; then
	echo "OCSSD device list is empty."
	echo "This test require that OCSSD_PCI_DEVICES environment variable to be set"
	echo "and point to OCSSD devices PCI BDF. You can specify multiple space"
	echo "separated BDFs in this case first one will be used."
	exit 1
fi

ocssd_original_dirver="$(basename $(readlink /sys/bus/pci/devices/$device/driver))"

trap "at_ftl_exit" SIGINT SIGTERM EXIT

# OCSSD is blacklisted so bind it to vfio/uio driver before testing
PCI_WHITELIST="$device" PCI_BLACKLIST="" DRIVER_OVERRIDE="" ./scripts/setup.sh

timing_enter ftl
timing_enter bdevperf

run_test suite $testdir/bdevperf.sh $device

timing_exit bdevperf

timing_enter restore
run_test suite $testdir/restore.sh $device
timing_exit restore

if [ $SPDK_TEST_FTL_EXTENDED -eq 1 ]; then
	$rootdir/test/app/bdev_svc/bdev_svc &
	bdev_svc_pid=$!

	trap "killprocess $bdev_svc_pid; exit 1" SIGINT SIGTERM EXIT

	waitforlisten $bdev_svc_pid
	uuid=$($rpc_py construct_ftl_bdev -b nvme0 -a $device -l 0-3 | jq -r '.uuid')
	killprocess $bdev_svc_pid

	trap - SIGINT SIGTERM EXIT

	timing_enter fio_extended
	run_test suite $testdir/fio.sh $device $uuid
	timing_exit fio_extended
fi

timing_exit ftl
