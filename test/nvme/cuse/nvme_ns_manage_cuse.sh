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

# Find bdf that supports Namespace Management
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
	echo "No NVMe device supporting Namespace management found"
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

function remove_all_namespaces() {
	info_print "delete all namespaces"
	active_nsids=$($NVME_CMD list-ns ${nvme_dev} | cut -f2 -d:)
	# Cant globally detach all namespaces ... must do so one by one
	for n in ${active_nsids}; do
		info_print "removing nsid=${n}"
		$NVME_CMD detach-ns ${nvme_dev} -n ${n} -c 0 || true
		$NVME_CMD delete-ns ${nvme_dev} -n ${n} || true
	done
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
	remove_all_namespaces
	nsid=$($NVME_CMD create-ns ${nvme_dev} -s ${size} -c ${size} -b ${blksize} | grep -o 'nsid:[0-9].*' | cut -f2 -d:)
	$NVME_CMD attach-ns ${nvme_dev} -n ${nsid} -c 0
	$NVME_CMD reset ${nvme_dev}

	$rootdir/scripts/setup.sh
}

function info_print() {
	echo "---"
	echo "$@"
	echo "---"
}

# Prepare controller
remove_all_namespaces

reset_nvme_if_aer_unsupported ${nvme_dev}
sleep 1

PCI_ALLOWED="${bdf}" $rootdir/scripts/setup.sh

$SPDK_BIN_DIR/spdk_tgt -m 0x3 &
spdk_tgt_pid=$!
trap 'kill -9 ${spdk_tgt_pid}; clean_up; exit 1' SIGINT SIGTERM EXIT

waitforlisten $spdk_tgt_pid

$rpc_py bdev_nvme_attach_controller -b Nvme0 -t PCIe -a ${bdf}
$rpc_py bdev_nvme_cuse_register -n Nvme0

ctrlr="/dev/spdk/nvme0"

sleep 1
[[ -c $ctrlr ]]

for dev in "${ctrlr}"n*; do
	[[ ! -c ${dev} ]]
done
sleep 1
nsids=()

for i in {1..2}; do
	info_print "create ns: nsze=10000 ncap=10000 flbias=0"
	nsid=$($NVME_CMD create-ns ${ctrlr} -s 10000 -c 10000 -f 0 | grep -o 'nsid:[0-9].*' | cut -f2 -d:)
	nsids+=(${nsid})
	info_print "attach ns: nsid=${nsid} controller=0"
	$NVME_CMD attach-ns ${ctrlr} -n ${nsid} -c 0

	reset_nvme_if_aer_unsupported ${ctrlr}
	sleep 1

	[[ -c "${ctrlr}n${nsid}" ]]
done

for n in "${nsids[@]}"; do
	info_print "detach ns: nsid=${n} controller=0"
	$NVME_CMD detach-ns ${ctrlr} -n ${n} -c 0 || true

	info_print "delete ns: nsid=${n}"
	$NVME_CMD delete-ns ${ctrlr} -n ${n} || true

	reset_nvme_if_aer_unsupported ${ctrlr}
	sleep 1

	[[ ! -c "${ctrlr}n${n}" ]]
done

# Here we should not have any cuse devices
for dev in "${ctrlr}"n*; do
	[[ ! -c ${dev} ]]
done

$rpc_py bdev_nvme_detach_controller Nvme0

sleep 1
[[ ! -c ${ctrlr} ]]

trap - SIGINT SIGTERM EXIT
killprocess $spdk_tgt_pid
clean_up
