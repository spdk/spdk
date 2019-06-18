#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh
source $testdir/common.sh

rpc_py=$rootdir/scripts/rpc.py

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

# Use first regular NVMe disk (non-OC) as non-volatile cache
nvme_disks=$($rootdir/scripts/gen_nvme.sh --json | jq -r \
	   "[.config[] | select(.params.traddr != \"$device\")] | .[].params.traddr")

for disk in $nvme_disks; do
	if has_separate_md $disk; then
		nv_cache=$disk
		break
	fi
done

if [ -z "$nv_cache" ]; then
	# TODO: once CI has devices with separate metadata support fail the test here
	echo "Couldn't find NVMe device to be used as non-volatile cache"
fi

timing_enter ftl
timing_enter bdevperf

run_test suite $testdir/bdevperf.sh $device

timing_exit bdevperf

timing_enter restore
run_test suite $testdir/restore.sh $device
if [ -n "$nv_cache" ]; then
	run_test suite $testdir/restore.sh -c $nv_cache $device
fi
timing_exit restore

if [ -n "$nv_cache" ]; then
	timing_enter dirty_shutdown
	run_test suite $testdir/dirty_shutdown.sh -c $nv_cache $device
	timing_exit dirty_shutdown
fi

timing_enter json
run_test suite $testdir/json.sh $device
timing_exit json

if [ $SPDK_TEST_FTL_EXTENDED -eq 1 ]; then
	timing_enter fio_basic
	run_test suite $testdir/fio.sh $device basic
	timing_exit fio_basic

	$rootdir/test/app/bdev_svc/bdev_svc &
	bdev_svc_pid=$!

	trap "killprocess $bdev_svc_pid; exit 1" SIGINT SIGTERM EXIT

	waitforlisten $bdev_svc_pid
	uuid=$($rpc_py construct_ftl_bdev -b nvme0 -a $device -l 0-3 | jq -r '.uuid')
	killprocess $bdev_svc_pid

	trap - SIGINT SIGTERM EXIT

	timing_enter fio_extended
	run_test suite $testdir/fio.sh $device extended $uuid
	timing_exit fio_extended
fi

timing_exit ftl
