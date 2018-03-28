#!/usr/bin/env bash
set -xe

basedir=$(readlink -f $(dirname $0))
MAKE="make -j$(( $(nproc)  * 2 ))"

if [[ $1 == "spdk_vhost_scsi" ]]; then
	devs=""
	for entry in /sys/block/sd*; do
		if grep -Eq '(INTEL|RAWSCSI|LIO-ORG)' $entry/device/vendor; then
			devs+="$(basename $entry) "
		fi
	done
elif [[ $1 == "spdk_vhost_blk" ]]; then
	devs=$(cd /sys/block; echo vd*)
fi

fs=$2

trap "exit 1" SIGINT SIGTERM EXIT

for fs in $fs; do
	for dev in $devs; do
		parted_cmd="parted -s /dev/${dev}"

		echo "INFO: Creating partition table on disk using: $parted_cmd mklabel gpt"
		$parted_cmd mklabel gpt
		$parted_cmd mkpart primary 2048s 100%
		sleep 2

		mkfs_cmd="mkfs.$fs"
		if [[ $fs == "ntfs" ]]; then
			mkfs_cmd+=" -f"
		fi
		mkfs_cmd+=" /dev/${dev}1"
		echo "INFO: Creating filesystem using: $mkfs_cmd"
		wipefs -a /dev/${dev}1
		$mkfs_cmd

		mkdir -p /mnt/${dev}dir
		mount -o sync /dev/${dev}1 /mnt/${dev}dir

		fio --name="integrity" --bsrange=4k-512k --iodepth=128 --numjobs=1 --direct=1 \
		--thread=1 --group_reporting=1 --rw=randrw --rwmixread=70 \
		--filename=/mnt/${dev}dir/test_file --verify=md5 --do_verify=1 \
		--verify_backlog=1024 --fsync_on_close=1 --runtime=20 --time_based=1 --size=512m

		# Print out space consumed on target device
		df -h /dev/$dev
	done

	for dev in $devs; do
		umount /mnt/${dev}dir
		rm -rf /mnt/${dev}dir

		stats=( $(cat /sys/block/$dev/stat) )
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
