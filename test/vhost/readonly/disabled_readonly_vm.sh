#!/usr/bin/env bash

set -e
BASE_DIR=$(readlink -f $(dirname $0))

function error()
{
	echo "==========="
	echo -e "ERROR: $@"
	echo "==========="
	exit 1
}

disk_name=$(cd /sys/block; echo vd*)
test_folder_name="readonly_test"
test_file_name="some_test_file"

if [ -z $disk_name ]; then
	error "No vhost-blk disk found!"
fi

if (( $(lsblk -r -n -o RO -d "/dev/$disk_name") == 1 )); then
	error "Vhost-blk disk is set as readonly!"
fi

parted -s /dev/$disk_name mklabel gpt
parted -s /dev/$disk_name mkpart primary 2048s 100%

if [[ $? != 0 ]]; then
	error "Failed to create partition!"
fi

partprobe
sleep 0.1
cat /proc/partitions | grep -q1 $disk_name"1"
if [[ $? != 0 ]]; then
	error "Partition not found!"
fi

echo "INFO: Creating file system"
mkfs.ext4 -F /dev/$disk_name"1"
if [[ $? != 0 ]]; then
	error "Failed to create file system"
fi

if [[ -d "$BASE_DIR/$test_folder_name" ]]; then
	echo "INFO: Found old test folder $test_folder_name"
	if mount | grep -q "$BASE_DIR/$test_folder_name"; then
		echo "INFO: Unmounting folder $test_folder_name"
		umount $BASE_DIR/$test_folder_name
	fi

	echo "INFO: Removing old test folder $test_folder_name"
	rm -rf $BASE_DIR/$test_folder_name
fi

mkdir $BASE_DIR/$test_folder_name
if [[ $? != 0 ]]; then
	error "Failed co create test folder $test_folder_name"
fi

echo "INFO: Mounting disk"
mount /dev/$disk_name"1" $BASE_DIR/$test_folder_name
if [[ $? != 0 ]]; then
	error "Failed to mount disk"
fi

echo "INFO: Creating a test file $test_file_name"
dd if=/dev/zero of=$BASE_DIR/$test_folder_name/$test_file_name bs=1M count=200

if [[ -d "$BASE_DIR/$test_folder_name" ]]; then
	if mount | grep -q "$BASE_DIR/$test_folder_name"; then
		echo "INFO: Unmouting folder $test_folder_name"
		umount "$BASE_DIR/$test_folder_name"
	fi

	echo "INFO: Removing folder $test_folder_name"
	rm -rf "$BASE_DIR/$test_folder_name"
fi

sleep 0.5
