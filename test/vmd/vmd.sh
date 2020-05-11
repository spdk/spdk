#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/scripts/common.sh
source $rootdir/test/common/autotest_common.sh

rpc_py=$rootdir/scripts/rpc.py
VMD_WHITELIST=()

function vmd_identify() {
	for bdf in $pci_devs; do
		$SPDK_EXAMPLE_DIR/identify -i 0 -V -r "trtype:PCIe traddr:$bdf"
	done
}

function vmd_perf() {
	for bdf in $pci_devs; do
		$SPDK_EXAMPLE_DIR/perf -q 128 -w read -o 12288 -t 1 -LL -i 0 -V -r "trtype:PCIe traddr:$bdf"
	done
}

function vmd_fio() {
	for bdf in $pci_devs; do
		fio_nvme $testdir/config/config.fio --filename="trtype=PCIe traddr=${bdf//:/.} ns=1"
	done
}

function vmd_bdev_svc() {
	$rootdir/test/app/bdev_svc/bdev_svc --wait-for-rpc &
	svcpid=$!
	trap 'killprocess $svcpid; exit 1' SIGINT SIGTERM EXIT

	# Wait until bdev_svc starts
	waitforlisten $svcpid

	$rpc_py enable_vmd
	$rpc_py framework_start_init

	for bdf in $pci_devs; do
		$rpc_py bdev_nvme_attach_controller -b NVMe_$bdf -t PCIe -a $bdf
	done

	trap - SIGINT SIGTERM EXIT
	killprocess $svcpid
}

# Re-run setup.sh script and only attach VMD devices to uio/vfio.
$rootdir/scripts/setup.sh reset

vmd_id=$(grep "PCI_DEVICE_ID_INTEL_VMD" $rootdir/include/spdk/pci_ids.h | awk -F"x" '{print $2}')

for bdf in $(iter_pci_dev_id 8086 $vmd_id); do
	if pci_can_use $bdf; then
		VMD_WHITELIST+=("$bdf")
	fi
done
PCI_WHITELIST="${VMD_WHITELIST[*]}" $rootdir/scripts/setup.sh

pci_devs=$($SPDK_BIN_DIR/spdk_lspci | grep "NVMe disk behind VMD" | awk '{print $1}')

if [[ -z "$pci_devs" ]]; then
	echo "Couldn't find any NVMe device behind a VMD."
	exit 1
fi

run_test "vmd_identify" vmd_identify
run_test "vmd_hello_world" $SPDK_EXAMPLE_DIR/hello_world -V
run_test "vmd_perf" vmd_perf
if [[ $CONFIG_FIO_PLUGIN == y ]]; then
	run_test "vmd_fio" vmd_fio
fi

run_test "vmd_bdev_svc" vmd_bdev_svc

# Re-run setup.sh again so that other tests may continue
$rootdir/scripts/setup.sh reset
$rootdir/scripts/setup.sh
