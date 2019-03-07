#!/usr/bin/env bash

set -e

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)

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
run_test suite $testdir/restore.sh $device $uuid
timing_exit restore

timing_exit ftl

trap - SIGINT SIGTERM EXIT
at_ftl_exit
