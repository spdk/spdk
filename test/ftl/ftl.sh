#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh
source $testdir/common.sh

rpc_py=$rootdir/scripts/rpc.py

function at_ftl_exit() {
	# restore original driver
	PCI_WHITELIST="$device" PCI_BLACKLIST="" DRIVER_OVERRIDE="$ocssd_original_dirver" $rootdir/scripts/setup.sh
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

# OCSSD is blacklisted so bind it to vfio/uio driver before testing
PCI_WHITELIST="$device" PCI_BLACKLIST="" DRIVER_OVERRIDE="" $rootdir/scripts/setup.sh

# Use first regular NVMe disk (non-OC) as non-volatile cache
nvme_disks=$($rootdir/scripts/gen_nvme.sh | jq -r \
	".config[] | select(.params.traddr != \"$device\").params.traddr")

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

run_test "ftl_bdevperf" $testdir/bdevperf.sh $device
run_test "ftl_bdevperf_append" $testdir/bdevperf.sh $device --use_append

run_test "ftl_restore" $testdir/restore.sh $device
if [ -n "$nv_cache" ]; then
	run_test "ftl_restore_nv_cache" $testdir/restore.sh -c $nv_cache $device
fi

if [ -n "$nv_cache" ]; then
	run_test "ftl_dirty_shutdown" $testdir/dirty_shutdown.sh -c $nv_cache $device
fi

run_test "ftl_json" $testdir/json.sh $device

if [ $SPDK_TEST_FTL_EXTENDED -eq 1 ]; then
	run_test "ftl_fio_basic" $testdir/fio.sh $device basic

	"$SPDK_BIN_DIR/spdk_tgt" --json <(gen_ftl_nvme_conf) &
	svcpid=$!

	trap 'killprocess $svcpid; exit 1' SIGINT SIGTERM EXIT

	waitforlisten $svcpid

	$rpc_py bdev_nvme_attach_controller -b nvme0 -a $device -t pcie
	$rpc_py bdev_ocssd_create -c nvme0 -b nvme0n1 -n 1
	uuid=$($rpc_py bdev_ftl_create -b ftl0 -d nvme0n1 | jq -r '.uuid')
	killprocess $svcpid

	trap - SIGINT SIGTERM EXIT

	run_test "ftl_fio_extended" $testdir/fio.sh $device extended $uuid
fi
