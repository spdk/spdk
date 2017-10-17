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

if [ -f $BASE_DIR/$test_folder_name/$test_file_name ]; then
	echo "INFO: Removing previously created file"
	rm $BASE_DIR/$test_folder_name/$test_file_name
		if [[ $? != 0 ]]; then
			error "Failed to delete file $test_file_name!"
		fi
else
	echo "INFO: Previously created file not found"
fi

echo "INFO: Removing folder and unmounting $test_folder_name"
umount "$BASE_DIR/$test_folder_name"
rm -rf "$BASE_DIR/$test_folder_name"

echo "INFO: Deleting partition"
echo -e "d\n1\nw" | fdisk /dev/$disk_name
if [[ $? != 0 ]]; then
	error "Failed to delete partition!"
fi
