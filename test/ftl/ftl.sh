#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh
source $testdir/common.sh

# The FTL tests are currently disabled, pending conversion to ZNS from OCSSD.
exit 0

function at_ftl_exit() {
	# restore original driver
	PCI_ALLOWED="$device" PCI_BLOCKED="" DRIVER_OVERRIDE="$ocssd_original_dirver" $rootdir/scripts/setup.sh
}

read -r device _ <<< "$OCSSD_PCI_DEVICES"

if [[ -z "$device" ]]; then
	echo "OCSSD device list is empty."
	echo "This test require that OCSSD_PCI_DEVICES environment variable to be set"
	echo "and point to OCSSD devices PCI BDF. You can specify multiple space"
	echo "separated BDFs in this case first one will be used."
	exit 1
fi

ocssd_original_dirver="$(basename $(readlink /sys/bus/pci/devices/$device/driver))"

trap 'at_ftl_exit' SIGINT SIGTERM EXIT

# OCSSD is blocked so bind it to vfio/uio driver before testing
PCI_ALLOWED="$device" PCI_BLOCKED="" DRIVER_OVERRIDE="" $rootdir/scripts/setup.sh

run_test "ftl_bdevperf" $testdir/bdevperf.sh $device
run_test "ftl_bdevperf_append" $testdir/bdevperf.sh $device --use_append
run_test "ftl_restore" $testdir/restore.sh $device
run_test "ftl_json" $testdir/json.sh $device
run_test "ftl_fio" "$testdir/fio.sh" "$device"
