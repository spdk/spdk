#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh
source $testdir/common.sh

rpc_py=$rootdir/scripts/rpc.py

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

# Use first regular NVMe disk (non-OC) as non-volatile cache
nvme_disks=$($rootdir/scripts/gen_nvme.sh | jq -r \
	".config[] | select(.params.traddr != \"$device\").params.traddr")

for disk in $nvme_disks; do
	nv_cache=$disk
	break
done

if [ -z "$nv_cache" ]; then
	echo "Couldn't find NVMe device to be used as non-volatile cache"
	exit 1
fi

if [[ -z $SPDK_TEST_FTL_NIGHTLY  ]]; then
	run_test "ftl_fio_basic" $testdir/fio.sh $device $nv_cache basic
fi

if [ $SPDK_TEST_FTL_EXTENDED -eq 1 ]; then
	run_test "ftl_fio_extended" $testdir/fio.sh $device $nv_cache extended
fi

if [ $SPDK_TEST_FTL_NIGHTLY -eq 1 ]; then
	run_test "ftl_recovery" $testdir/jenkins/jenkins-ftl-test-nightly.sh $device $nv_cache
fi
