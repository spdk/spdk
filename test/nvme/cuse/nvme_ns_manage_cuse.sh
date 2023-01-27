#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2020 Intel Corporation
#  All rights reserved.
#
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source "$testdir/common.sh"

$rootdir/scripts/setup.sh reset
scan_nvme_ctrls

# Find bdf that supports Namespace Management
for ctrl in "${!ctrls[@]}"; do
	# Check Optional Admin Command Support for Namespace Management
	(($(get_nvme_ctrl_feature "$ctrl" oacs) & 0x8)) && nvme_name=$ctrl && break
done

if [[ -z $nvme_name ]]; then
	echo "No NVMe device supporting Namespace management found"
	$rootdir/scripts/setup.sh
	exit 1
fi

nvme_dev=/dev/${nvme_name}
bdf=${bdfs["$nvme_name"]}
nsids=($(get_nvme_nss "$nvme_name"))

# Detect supported features and configuration
oaes=$(get_nvme_ctrl_feature "$nvme_name" oaes)
aer_ns_change=$((oaes & 0x100))
cntlid=$(get_nvme_ctrl_feature "$nvme_name")

function reset_nvme_if_aer_unsupported() {
	if [[ "$aer_ns_change" -eq "0" ]]; then
		sleep 1
		$NVME_CMD reset "$1" || true
	fi
}

function remove_all_namespaces() {
	info_print "delete all namespaces"
	# Cant globally detach all namespaces ... must do so one by one
	for nsid in "${nsids[@]}"; do
		info_print "removing nsid=${nsid}"
		$NVME_CMD detach-ns ${nvme_dev} -n ${nsid} -c ${cntlid} || true
		$NVME_CMD delete-ns ${nvme_dev} -n ${nsid} || true
	done
}

function clean_up() {
	"$rootdir/scripts/setup.sh" reset
	remove_all_namespaces

	echo "Restoring $nvme_dev..."
	for nsid in "${nsids[@]}"; do
		ncap=$(get_nvme_ns_feature "$nvme_name" "$nsid" ncap)
		nsze=$(get_nvme_ns_feature "$nvme_name" "$nsid" nsze)
		lbaf=$(get_active_lbaf "$nvme_name" "$nsid")
		$NVME_CMD create-ns ${nvme_dev} -s ${nsze} -c ${ncap} -f ${lbaf}
		$NVME_CMD attach-ns ${nvme_dev} -n ${nsid} -c ${cntlid}
		$NVME_CMD reset ${nvme_dev}
	done

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

for nsid in "${nsids[@]}"; do
	info_print "create ns: nsze=10000 ncap=10000 flbias=0"
	$NVME_CMD create-ns ${ctrlr} -s 10000 -c 10000 -f 0
	info_print "attach ns: nsid=${nsid} controller=${cntlid}"
	$NVME_CMD attach-ns ${ctrlr} -n ${nsid} -c ${cntlid}
	reset_nvme_if_aer_unsupported ${ctrlr}
	sleep 1
	[[ -c "${ctrlr}n${nsid}" ]]
	info_print "detach ns: nsid=${nsid} controller=${cntlid}"
	$NVME_CMD detach-ns ${ctrlr} -n ${nsid} -c ${cntlid}
	info_print "delete ns: nsid=${nsid}"
	$NVME_CMD delete-ns ${ctrlr} -n ${nsid}
	reset_nvme_if_aer_unsupported ${ctrlr}
	sleep 1
	[[ ! -c "${ctrlr}n${nsid}" ]]
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
