#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)

source $rootdir/scripts/common.sh
source $rootdir/test/common/autotest_common.sh

function ftl_kill() {
	rm -f $testdir/.testfile_*

	# restore orginal driver
	export DRIVER_OVERRIDE="$ocssd_original_dirver"
	./scripts/setup.sh
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

# OCSSD is blacklistened so bin it to vfio/uio driver before testing
export PCI_WHITELIST="$device"
export PCI_BLACKLIST=""
export DRIVER_OVERRIDE=""

set -e

trap "ftl_kill; exit 1" SIGINT SIGTERM EXIT
./scripts/setup.sh

timing_enter ftl
timing_enter fio

run_test suite $testdir/fio.sh $device

timing_exit fio

timing_enter restore
run_test suite $testdir/restore.sh $device $uuid
timing_exit restore

timing_exit ftl

trap - SIGINT SIGTERM EXIT
ftl_kill
