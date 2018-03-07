#!/usr/bin/env bash

set -x
BASE_DIR=$(readlink -f $(dirname $0))

disk_name="vda"
test_folder_name="readonly_test"
test_file_name="some_test_file"

function error()
{
	echo "==========="
	echo -e "ERROR: $@"
	echo "==========="
	umount "$test_folder_name"
	rm -rf "$BASE_DIR/$test_folder_name"
	exit 1
}

if [[ ! -d "/sys/block/$disk_name" ]]; then
	error "No vhost-blk disk found!"
fi

if (( $(lsblk -r -n -o RO -d "/dev/$disk_name") == 0 )); then
	error "Vhost-blk disk is not set as readonly!"
fi

echo "INFO: Found vhost-blk disk with readonly flag"
if [[ ! -b "/dev/$disk_name"1"" ]]; then
	error "Partition not found!"
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
truncate -s "200M" $test_folder_name/$test_file_name"_on_readonly"
if [[ $? == 0 ]]; then
	error "Created a file on a readonly disk!"
fi

if [[ -f $test_folder_name/$test_file_name ]]; then
	echo "INFO: Trying to delete previously created file"
	rm $test_folder_name/$test_file_name
	if [[ $? == 0 ]]; then
		error "Deleted a file from a readonly disk!"
	fi
else
	error "Previously created file not found!"
fi

echo "INFO: Copying file from readonly disk"
cp $test_folder_name/$test_file_name $BASE_DIR
if ! rm $BASE_DIR/$test_file_name; then
	error "Copied file from a readonly disk was not found!"
fi

umount "$test_folder_name"
rm -rf "$BASE_DIR/$test_folder_name"
echo "INFO: Trying to create file system on a readonly disk"
if mkfs.ext4 -F /dev/$disk_name"1"; then
	error "Created file system on a readonly disk!"
fi

echo "INFO: Trying to delete partition from readonly disk"
if echo -e "d\n1\nw" | fdisk /dev/$disk_name; then
	error "Deleted partition from readonly disk!"
fi
