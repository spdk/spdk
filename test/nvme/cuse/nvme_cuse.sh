#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/scripts/common.sh
source $rootdir/test/common/autotest_common.sh

if [[ $(uname) != "Linux" ]]; then
	echo "NVMe cuse tests only supported on Linux"
	exit 1
fi

modprobe cuse
run_test "nvme_cuse_app" $testdir/cuse
run_test "nvme_cuse_rpc" $testdir/nvme_cuse_rpc.sh
run_test "nvme_cli_cuse" $testdir/spdk_nvme_cli_cuse.sh
run_test "nvme_smartctl_cuse" $testdir/spdk_smartctl_cuse.sh

# Only run Namespace managment test case when such device is present
bdfs=$(get_nvme_bdfs)

$rootdir/scripts/setup.sh reset
sleep 1

# Find bdf that supports Namespace managment
for bdf in $bdfs; do
	nvme_name=$(get_nvme_ctrlr_from_bdf ${bdf})
	if [[ -z "$nvme_name" ]]; then
		continue
	fi

	# Check Optional Admin Command Support for Namespace Management
	oacs=$(nvme id-ctrl /dev/${nvme_name} | grep oacs | cut -d: -f2)
	oacs_ns_manage=$((oacs & 0x8))

	if [[ "$oacs_ns_manage" -ne 0 ]]; then
		break
	fi
done

if [[ "$oacs_ns_manage" -ne 0 ]]; then
	run_test "nvme_ns_manage_cuse" $testdir/nvme_ns_manage_cuse.sh
fi
$rootdir/scripts/setup.sh

rmmod cuse
