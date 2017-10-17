#!/usr/bin/env bash

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

if [[ -z $disk_name ]]; then
	error "No vhost-blk disk found!"
fi

if (( $(lsblk -r -n -o RO -d "/dev/$disk_name") == 0 )); then
	error "Vhost-blk disk is not set as readonly!"
fi

echo "INFO: Found vhost-blk disk with readonly flag"
if ! cat /proc/partitions | grep -qi $disk_name"1";then
	error "Previously created partition wasn't found!"
fi

if [[ -d "$BASE_DIR/$test_folder_name" ]];then
	echo "INFO: Found old test folder $test_folder_name"
	if mount | grep -q "$test_folder_name"; then
		echo "INFO: Unmouting folder $test_folder_name"
		umount $test_folder_name
	fi

	echo "INFO: Removing old test folder $test_folder_name"
	rm -rf $BASE_DIR/$test_folder_name
fi

mkdir $BASE_DIR/$test_folder_name
if [[ $? != 0 ]]; then
	error "Failed to create test folder $test_folder_name"
fi

echo "INFO: Mounting partition"
mount /dev/$disk_name"1" $BASE_DIR/$test_folder_name
if [[ $? != 0 ]]; then
	error "Failed to mount partition $disk_name""1"
fi

echo "INFO: Trying to create file on readonly disk"
dd if=/dev/zero of=$BASE_DIR/$test_folder_name/$test_file_name"_on_readonly" bs=1M count=200
if [[ $? == 0 ]]; then
	error "Created a file on a readonly disk!"
fi

if [ -f $BASE_DIR/$test_folder_name/$test_file_name ]; then
	echo "INFO: Trying to delete previously created file"
	rm $BASE_DIR/$test_folder_name/$test_file_name
		if [[ $? == 0 ]]; then
			error "Deleted a file from a readonly disk!"
		fi
else
	error "Previously created file not found!"
fi

echo "INFO: Copying file from readonly disk"
cp $BASE_DIR/$test_folder_name/$test_file_name $BASE_DIR
if [[ $? != 0 ]]; then
	error "Copy from a readonly disk failed!"
fi

rm $BASE_DIR/$test_file_name
if [[ $? != 0 ]]; then
	echo "INFO: Failed to remove copied file"
fi

if [ -d "$BASE_DIR/$test_folder_name" ];then
	if mount | grep -q "$test_folder_name"; then
		echo "INFO: Unmouting folder $test_folder_name"
		umount "$BASE_DIR/$test_folder_name"
	fi

	echo "INFO: Removing folder $test_folder_name"
	rm -rf "$BASE_DIR/$test_folder_name"
fi

mkfs.ext4 -F /dev/$disk_name"1"
if [[ $? == 0 ]]; then
	error "Created file system on a readonly disk!"
fi

echo -e "d\n1\nw" | fdisk /dev/$disk_name
if [[ $? == 0 ]]; then
	error "Deleted partition from readonly disk!"
fi

sleep 0.5
