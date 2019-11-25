#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/scripts/common.sh
source $rootdir/test/common/autotest_common.sh

timing_enter nvme_cli_cuse

SMARTCTL_CMD='smartctl -d nvme,0xffffffff'
rpc_py=$rootdir/scripts/rpc.py

bdf=$(iter_pci_class_code 01 08 02 | head -1)

PCI_WHITELIST="${bdf}" $rootdir/scripts/setup.sh reset
sleep 1
bdf_sysfs_path=$( readlink -f /sys/class/nvme/nvme* | grep "$bdf/nvme/nvme" )
if [ -z "$bdf_sysfs_path" ]; then
	echo "Cannot bind kernel driver to ${bdf}"
	return 1
fi
nvme_name=$( basename $bdf_sysfs_path )

KERNEL_SMART_JSON=$( ${SMARTCTL_CMD} --json=g -a /dev/${nvme_name} | sort || true )

$rootdir/scripts/setup.sh

$rootdir/app/spdk_tgt/spdk_tgt -m 0x3 &
spdk_tgt_pid=$!
trap 'kill -9 ${spdk_tgt_pid}; exit 1' SIGINT SIGTERM EXIT

waitforlisten $spdk_tgt_pid

$rpc_py bdev_nvme_attach_controller -b Nvme0 -t PCIe -a ${bdf}
$rpc_py bdev_nvme_cuse_register -n Nvme0

sleep 5

if [ ! -c /dev/spdk/nvme0 ]; then
	return 1
fi

CUSE_SMART_JSON=$( ${SMARTCTL_CMD} --json=g -a /dev/spdk/nvme0 | sort || true )

diff -y --suppress-common-lines <(echo "$KERNEL_SMART_JSON") <(echo "$CUSE_SMART_JSON")

#${SMARTCTL_CMD} -j -i /dev/spdk/nvme0n1 || true

#${SMARTCTL_CMD} -j -c /dev/spdk/nvme0 || true

#${SMARTCTL_CMD} -j -H /dev/spdk/nvme0 || true

#${SMARTCTL_CMD} -j -A /dev/spdk/nvme0 || true

#${SMARTCTL_CMD} -j -l error /dev/spdk/nvme0 || true

#${SMARTCTL_CMD} -j -t select,10-20 /dev/spdk/nvme0n1 || true

#${SMARTCTL_CMD} -j -t select,10+11 /dev/spdk/nvme0n1 || true

#${SMARTCTL_CMD} -j -x /dev/spdk/nvme0 || true

$rpc_py bdev_nvme_detach_controller Nvme0
sleep 1
if [ -c /dev/spdk/nvme1 ]; then
	return 1
fi

trap - SIGINT SIGTERM EXIT
killprocess $spdk_tgt_pid

report_test_completion spdk_nvme_cli_cuse
timing_exit nvme_cli_cuse
