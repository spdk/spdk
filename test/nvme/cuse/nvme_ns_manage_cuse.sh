#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/scripts/common.sh
source $rootdir/test/common/autotest_common.sh

NVME_CMD="/usr/local/src/nvme-cli/nvme"

rpc_py=$rootdir/scripts/rpc.py

$rootdir/scripts/setup.sh
sleep 1

bdfs=$(get_nvme_bdfs)

$rootdir/scripts/setup.sh reset

# Find bdf that supports Namespace Managment
for bdf in $bdfs; do
	nvme_name=$(get_nvme_ctrlr_from_bdf ${bdf})
	if [[ -z "$nvme_name" ]]; then
		continue
	fi

	# Check Optional Admin Command Support for Namespace Management
	oacs=$($NVME_CMD id-ctrl /dev/${nvme_name} | grep oacs | cut -d: -f2)
	oacs_ns_manage=$((oacs & 0x8))

	if [[ "$oacs_ns_manage" -ne 0 ]]; then
		break
	fi
done

if [[ "${nvme_name}" == "" ]] || [[ "$oacs_ns_manage" -eq 0 ]]; then
	echo "No NVMe device supporting Namespace managment found"
	$rootdir/scripts/setup.sh
	exit 1
fi

nvme_dev=/dev/${nvme_name}

# Detect supported features and configuration
oaes=$($NVME_CMD id-ctrl ${nvme_dev} | grep oaes | cut -d: -f2)
aer_ns_change=$((oaes & 0x100))

function reset_nvme_if_aer_unsupported() {
	if [[ "$aer_ns_change" -eq "0" ]]; then
		sleep 1
		$NVME_CMD reset "$1" || true
	fi
}

function clean_up() {
	$rootdir/scripts/setup.sh reset

	# This assumes every NVMe controller contains single namespace,
	# encompassing Total NVM Capacity and formatted as 512 block size.
	# 512 block size is needed for test/vhost/vhost_boot.sh to
	# succesfully run.

	tnvmcap=$($NVME_CMD id-ctrl ${nvme_dev} | grep tnvmcap | cut -d: -f2)
	blksize=512

	size=$((tnvmcap / blksize))

	echo "Restoring $nvme_dev..."
	$NVME_CMD detach-ns ${nvme_dev} -n 0xffffffff -c 0 || true
	$NVME_CMD delete-ns ${nvme_dev} -n 0xffffffff || true
	$NVME_CMD create-ns ${nvme_dev} -s ${size} -c ${size} -b ${blksize}
	$NVME_CMD attach-ns ${nvme_dev} -n 1 -c 0
	$NVME_CMD reset ${nvme_dev}

	$rootdir/scripts/setup.sh
}

function info_print() {
	echo "---"
	echo "$@"
	echo "---"
}

# Prepare controller
info_print "delete all namespaces"
$NVME_CMD detach-ns ${nvme_dev} -n 0xffffffff -c 0 || true
$NVME_CMD delete-ns ${nvme_dev} -n 0xffffffff || true

reset_nvme_if_aer_unsupported ${nvme_dev}
sleep 1

PCI_WHITELIST="${bdf}" $rootdir/scripts/setup.sh

$SPDK_BIN_DIR/spdk_tgt -m 0x3 &
spdk_tgt_pid=$!
trap 'kill -9 ${spdk_tgt_pid}; clean_up; exit 1' SIGINT SIGTERM EXIT

waitforlisten $spdk_tgt_pid

$rpc_py bdev_nvme_attach_controller -b Nvme0 -t PCIe -a ${bdf}
$rpc_py bdev_nvme_cuse_register -n Nvme0

sleep 1
[[ -c /dev/spdk/nvme0 ]]

for dev in /dev/spdk/nvme0n*; do
	[[ ! -c ${dev} ]]
done

info_print "create ns: nsze=10000 ncap=10000 flbias=0"
$NVME_CMD create-ns /dev/spdk/nvme0 -s 10000 -c 10000 -f 0

info_print "attach ns: nsid=1 controller=0"
$NVME_CMD attach-ns /dev/spdk/nvme0 -n 1 -c 0

reset_nvme_if_aer_unsupported /dev/spdk/nvme0
sleep 1

[[ -c /dev/spdk/nvme0n1 ]]

info_print "create ns: nsze=10000 ncap=10000 flbias=0"
$NVME_CMD create-ns /dev/spdk/nvme0 -s 10000 -c 10000 -f 0

info_print "attach ns: nsid=2 controller=0"
$NVME_CMD attach-ns /dev/spdk/nvme0 -n 2 -c 0

reset_nvme_if_aer_unsupported /dev/spdk/nvme0
sleep 1

[[ -c /dev/spdk/nvme0n2 ]]

info_print "detach ns: nsid=2 controller=0"
$NVME_CMD detach-ns /dev/spdk/nvme0 -n 2 -c 0 || true

info_print "delete ns: nsid=2"
$NVME_CMD delete-ns /dev/spdk/nvme0 -n 2 || true

reset_nvme_if_aer_unsupported /dev/spdk/nvme0
sleep 1

[[ ! -c /dev/spdk/nvme0n2 ]]

info_print "detach ns: nsid=1 controller=0"
$NVME_CMD detach-ns /dev/spdk/nvme0 -n 1 -c 0 || true

info_print "delete ns: nsid=1"
$NVME_CMD delete-ns /dev/spdk/nvme0 -n 1 || true

reset_nvme_if_aer_unsupported /dev/spdk/nvme0
sleep 1

# Here we should not have any cuse devices
for dev in /dev/spdk/nvme0n*; do
	[[ ! -c ${dev} ]]
done

$rpc_py bdev_nvme_detach_controller Nvme0

sleep 1
[[ ! -c /dev/spdk/nvme0 ]]

trap - SIGINT SIGTERM EXIT
killprocess $spdk_tgt_pid
clean_up
