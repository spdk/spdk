#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2023 Intel Corporation
#  All rights reserved.

usage() {
	echo "Control LED on an NVMe bdev with NPEM capability."
	echo "Usage: $(basename $0) [BDEV_NAME] [LED_STATE]"
	echo ""
	echo "BDEV_NAME: The name of the SPDK NVMe bdev."
	echo "LED_STATE: The desired state of the LED. If omitted, the current state will be shown."
	echo "           Consult ledctl documentation for a list of supported states."
}

if [[ "$#" -lt 1 || "$#" -gt 2 ]]; then
	usage
	exit 1
fi

if ! command -v "ledctl" > /dev/null 2>&1; then
	echo "ERROR: ledctl is not found." >&2
	exit 1
fi

if ! ledctl --help | grep -q -- "--set-slot"; then
	echo "ERROR: The installed version of ledctl does not support the --set-slot command." >&2
	exit 1
fi

scriptdir=$(dirname $0)
bdev_name=$1
led_state=$2

# Find the PCI address of the nvme bdev
if ! bdev_info=$($scriptdir/rpc.py bdev_get_bdevs -b $bdev_name | jq -r '.[0]' 2> /dev/null); then
	echo "ERROR: bdev $bdev_name not found." >&2
	exit 1
fi

nvme_info=$(echo $bdev_info | jq -r '.driver_specific["nvme"] | select(.)')
if [ -z "$nvme_info" ]; then
	echo "ERROR: $bdev_name is not an nvme bdev." >&2
	exit 1
fi

nvme_pci_addr=$(echo $nvme_info | jq -r '.[0].pci_address | select(.)')
if [ -z "$nvme_pci_addr" ]; then
	echo "ERROR: $bdev_name pci address unknown." >&2
	exit 1
fi

# Get a list of slots recognized by ledctl
npem_slots=$(ledctl -P -c NPEM 2> /dev/null | awk '{print $2}')

# Get the slot that the nvme device is attached to
devpath=$(realpath --relative-to=/sys/devices /sys/bus/pci/devices/$nvme_pci_addr)

while [ "$devpath" != "." ]; do
	dev=$(basename $devpath)

	if echo $npem_slots | grep -wq $dev; then
		slot=$dev
		break
	fi

	devpath=$(dirname $devpath)
done

if [ -z "$slot" ]; then
	echo "ERROR: $bdev_name not recognized by ledctl as NPEM-capable." >&2
	exit 1
fi

# Pass the slot to ledctl
if [ -z "$led_state" ]; then
	ledctl -G -c NPEM -p $slot
else
	ledctl -S -c NPEM -p $slot -s $led_state
fi
