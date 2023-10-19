#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2023 Intel Corporation
#  All rights reserved.

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source "$testdir/common.sh"

# Give the devices back to the kernel at the end
trap 'killprocess $spdk_tgt_pid; "$rootdir/scripts/setup.sh" reset' EXIT

nvme() {
	# Apply some custom filters to align output between the plugin's listing and base nvme-cli's
	/usr/local/src/nvme-cli-plugin/nvme "$@" \
		| sed \
			-e 's#nqn.\+ ##g' \
			-e 's#"SubsystemNQN.*##g' \
			-e 's#NQN=.*##g' \
			-e 's#/dev\(/spdk\)\?/##g' \
			-e 's#ng#nvme##g' \
			-e 's#-subsys##g' \
			-e 's#PCIE#pcie#g' \
			-e 's#(null)#live#g'
	((PIPESTATUS[0] == 0))
}

kernel_out=()
cuse_out=()

rpc_py=$rootdir/scripts/rpc.py

# We need to make sure these tests don't discriminate against the PCI_BLOCKED devices
# since nvme-cli doesn't really care - to make sure all outputs are aligned, we need to
# include all the devices that we can find.
export PCI_BLOCKED=""

"$rootdir/scripts/setup.sh" reset
scan_nvme_ctrls

kernel_out[0]=$(nvme list)
kernel_out[1]=$(nvme list -v)
kernel_out[2]=$(nvme list -v -o json)
kernel_out[3]=$(nvme list-subsys)

$rootdir/scripts/setup.sh

$SPDK_BIN_DIR/spdk_tgt &
spdk_tgt_pid=$!

waitforlisten $spdk_tgt_pid

# Keep ctrls in order as cuse will always start registering with id 0 regardless of the ctrl's name
for ctrl in "${ordered_ctrls[@]}"; do
	$rpc_py bdev_nvme_attach_controller -b "$ctrl" -t PCIe -a "${bdfs["$ctrl"]}"
	$rpc_py bdev_nvme_cuse_register -n "$ctrl"

done

$rpc_py bdev_get_bdevs
$rpc_py bdev_nvme_get_controllers

cuse_out[0]=$(nvme spdk list)
cuse_out[1]=$(nvme spdk list -v)
cuse_out[2]=$(nvme spdk list -v -o json)
cuse_out[3]=$(nvme spdk list-subsys)

# plugin does not support json output for the list-subsys
[[ $(nvme spdk list-subsys -v -o json 2>&1) == "Json output format is not supported." ]]

diff -ub \
	<(printf '%s\n' "${kernel_out[@]}") \
	<(printf '%s\n' "${cuse_out[@]}")
