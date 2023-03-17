#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2016 Intel Corporation
#  All rights reserved.
#
set -xe

cleanup() {
	local _devs=()

	((${#devs[@]} > 0)) || return 0

	_devs=("${devs[@]/#//dev/}")

	umount "${_devs[@]}" || :
	wipefs --all --force "${_devs[@]}" || :
}

MAKE="make -j$(($(nproc) * 2))"

devs=()
if [[ $1 == "spdk_vhost_scsi" ]]; then
	for entry in /sys/block/sd*; do
		if [[ $(< "$entry/device/vendor") =~ (INTEL|RAWSCSI|LIO-ORG) ]]; then
			devs+=("${entry##*/}")
		fi
	done
elif [[ $1 == "spdk_vhost_blk" ]]; then
	devs=(/sys/block/vd*)
fi

fs=$2
devs=("${devs[@]##*/}")

trap "cleanup; exit 1" SIGINT SIGTERM EXIT

for fs in $fs; do
	for dev in "${devs[@]}"; do
		[[ -b /dev/$dev ]]
		wipefs --all --force "/dev/$dev"
		echo "INFO: Creating partition table on $dev disk"
		parted "/dev/$dev" -s mklabel gpt mkpart SPDK_TEST 2048s 100%
		sleep 1s
		wipefs --all --force "/dev/${dev}1"
		echo "INFO: Creating filesystem on /dev/${dev}1"

		if [[ $fs == ext4 ]]; then
			"mkfs.$fs" -F "/dev/${dev}1"
		else
			"mkfs.$fs" -f "/dev/${dev}1"
		fi

		mkdir -p /mnt/${dev}dir
		mount -o sync /dev/${dev}1 /mnt/${dev}dir

		fio --name="integrity" --bsrange=4k-512k --iodepth=128 --numjobs=1 --direct=1 \
			--thread=1 --group_reporting=1 --rw=randrw --rwmixread=70 \
			--filename=/mnt/${dev}dir/test_file --verify=md5 --do_verify=1 \
			--verify_backlog=1024 --fsync_on_close=1 --runtime=20 --time_based=1 \
			--size=512m --verify_state_save=0

		# Print out space consumed on target device
		df -h /dev/$dev

		umount /mnt/${dev}dir
		rm -rf /mnt/${dev}dir
		stats=($(cat /sys/block/$dev/stat))
		echo ""
		echo "$dev stats"
		printf "READ  IO cnt: % 8u merges: % 8u sectors: % 8u ticks: % 8u\n" \
			${stats[0]} ${stats[1]} ${stats[2]} ${stats[3]}
		printf "WRITE IO cnt: % 8u merges: % 8u sectors: % 8u ticks: % 8u\n" \
			${stats[4]} ${stats[5]} ${stats[6]} ${stats[7]}
		printf "in flight: % 8u io ticks: % 8u time in queue: % 8u\n" \
			${stats[8]} ${stats[9]} ${stats[10]}
		echo ""
	done
done

trap - SIGINT SIGTERM EXIT
cleanup
