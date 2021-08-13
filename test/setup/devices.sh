#!/usr/bin/env bash
testdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$testdir/../../")
source "$testdir/common.sh"

shopt -s nullglob

cleanup() {
	cleanup_nvme
	cleanup_dm

	if [[ -b /dev/$test_disk ]]; then
		wipefs --all "/dev/$test_disk"
	fi
}

cleanup_nvme() {
	if mountpoint -q "$nvme_mount"; then
		umount "$nvme_mount"
	fi

	if [[ -b /dev/$nvme_disk_p ]]; then
		wipefs --all "/dev/$nvme_disk_p"
	fi
	if [[ -b /dev/$nvme_disk ]]; then
		wipefs --all "/dev/$nvme_disk"
	fi
}

cleanup_dm() {
	if mountpoint -q "$dm_mount"; then
		umount "$dm_mount"
	fi
	if [[ -L /dev/mapper/$dm_name ]]; then
		dmsetup remove --force "$dm_name"
	fi
	if [[ -b /dev/$pv0 ]]; then
		wipefs --all "/dev/$pv0"
	fi
	if [[ -b /dev/$pv1 ]]; then
		wipefs --all "/dev/$pv1"
	fi
}

verify() {
	local dev=$1
	local mounts=$2
	local mount_point=$3
	local test_file=$4

	local found=0

	: > "$test_file"

	local pci status
	while read -r pci _ _ status; do
		if [[ $pci == "$dev" && \
			$status == *"Active mountpoints on $mounts"* ]]; then
			found=1
		fi
	done < <(PCI_ALLOWED="$dev" setup output config)
	((found == 1))

	# Does the mount still exist?
	mountpoint -q "$mount_point"
	# Does the test file still exist?
	[[ -e $test_file ]]
	rm "$test_file"
}

nvme_mount() {
	# Agenda 1:
	# - Create single partition on the nvme drive
	# - Install ext4 fs on the first partition
	# - Mount the partition
	# - Run tests and check if setup.sh skipped
	#   nvme controller given block device is
	#   bound to.

	# Agenda 2:
	# - Install ext4 on the entire nvme drive
	# - Mount the drive
	#   Run tests and check if setup.sh skipped
	#   nvme controller given block device is
	#   bound to.

	# Keep scope of all the variables global to make the cleanup process easier.

	nvme_disk=$test_disk
	nvme_disk_p=${nvme_disk}p1
	nvme_mount=$SPDK_TEST_STORAGE/nvme_mount
	nvme_dummy_test_file=$nvme_mount/test_nvme

	# Agenda 1
	partition_drive "$nvme_disk" 1
	mkfs "/dev/$nvme_disk_p" "$nvme_mount"

	verify \
		"${blocks_to_pci["$nvme_disk"]}" \
		"$nvme_disk:$nvme_disk_p" \
		"$nvme_mount" \
		"$nvme_dummy_test_file"

	cleanup_nvme

	# Agenda 2
	mkfs "/dev/$nvme_disk" "$nvme_mount" 1024M

	verify \
		"${blocks_to_pci["$nvme_disk"]}" \
		"$nvme_disk:$nvme_disk" \
		"$nvme_mount" \
		"$nvme_dummy_test_file"

	# All done, final cleanup
	cleanup_nvme
}

dm_mount() {
	# Agenda:
	#  - Create two partitions on the nvme drive
	#  - Create dm device consisting of half of
	#    the size of each partition.
	#  - Install ext4 fs on the dm device
	#  - Mount dm device
	#  - Run tests and check if setup.sh skipped
	#    nvme controller given block devices are
	#    bound to.

	# Keep scope of all the variables global to make the cleanup process easier.

	pv=$test_disk
	pv0=${pv}p1
	pv1=${pv}p2

	partition_drive "$pv"

	dm_name=nvme_dm_test
	dm_mount=$SPDK_TEST_STORAGE/dm_mount
	dm_dummy_test_file=$dm_mount/test_dm

	# Each partition is 1G in size, join their halfs
	dmsetup create "$dm_name" <<- DM_TABLE
		0 1048576 linear /dev/$pv0 0
		1048576 1048576 linear /dev/$pv1 0
	DM_TABLE

	[[ -e /dev/mapper/$dm_name ]]
	dm=$(readlink -f "/dev/mapper/$dm_name")
	dm=${dm##*/}

	[[ -e /sys/class/block/$pv0/holders/$dm ]]
	[[ -e /sys/class/block/$pv1/holders/$dm ]]

	mkfs "/dev/mapper/$dm_name" "$dm_mount"

	verify \
		"${blocks_to_pci["$pv"]}" \
		"$pv:$dm_name" \
		"$dm_mount" \
		"$dm_dummy_test_file"

	# All done, start tiding up
	cleanup_dm
}

trap "cleanup" EXIT

setup reset

get_zoned_devs

declare -a blocks=()
declare -A blocks_to_pci=()
min_disk_size=$((1024 ** 3 * 2)) # 2GB

for block in "/sys/block/nvme"*; do
	pci=$(readlink -f "$block/device/device")
	pci=${pci##*/}
	[[ ${zoned_devs[*]} == *"$pci"* ]] && continue
	if ! block_in_use "${block##*/}" && (($(sec_size_to_bytes "${block##*/}") >= min_disk_size)); then
		blocks+=("${block##*/}")
		blocks_to_pci["${block##*/}"]=$pci
	fi
done
((${#blocks[@]} > 0))

declare -r test_disk=${blocks[0]}

run_test "nvme_mount" nvme_mount
run_test "dm_mount" dm_mount
