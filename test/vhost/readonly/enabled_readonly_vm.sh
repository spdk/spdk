#!/usr/bin/env bash
set -x

function error()
{
	echo "==========="
	echo -e "ERROR: $@"
	echo "==========="
	exit 1
}

disk_name=$(shopt -s nullglob; cd /sys/block; echo vd*)
test_folder_name="readonly_test"
test_file_name="some_test_file"

if [ -z $disk_name ]; then
	error "No vhost-blk disk found!"
fi

if (( $(lsblk -r -n -o RO -d "/dev/$disk_name") == 0 )); then
	error "Vhost-blk disk is not set as readonly!"
fi

echo "Found vhost-blk disk with readonly flag"

if ! cat /proc/partitions | grep -q1 $disk_name"1";then
	error "Preaviously created partition wasn't found!"
fi

if [ -d "$test_folder_name" ];then
	echo "Found old test folder $test_folder_name"
	if mount | grep -q "$test_folder_name"; then
		echo "Unmouting folder $test_folder_name"
		umount $test_folder_name
	fi

	echo "Removing old test folder $test_folder_name"
	rm -rf $test_folder_name
fi

mkdir $test_folder_name
if [ $? != 0 ]; then
	error "Failed co create test folder $test_folder_name"
fi

mount /dev/$disk_name"1" $test_folder_name
if [ $? != 0 ]; then
	error "Failed to mount partition $disk_name""1"
fi

dd if=/dev/zero of=./$test_folder_name/$test_file_name"_on_readonly" bs=1M count=200
if [ $? == 0 ]; then
	error "Created a file on a readonly disk!"
fi

if [ -f $test_folder_name/$test_file_name ]; then
	echo "Trying to delete previously created file"
	rm $test_folder_name/$test_file_name
		if [ $? == 0 ]; then
			error "Deleted a file from a readonly disk!"
		fi
else
	error "Previously created file not found!"
fi

cp $test_folder_name/$test_file_name /root
if [ $? != 0 ]; then
	error "Copy from a readonly disk failed"
fi

rm $test_file_name
if [ $? != 0 ]; then
	echo "Failed to remove file copied file"
fi

echo "Unmouting partition"
if [ -d "$test_folder_name" ];then
	if mount | grep -q "$test_folder_name"; then
		echo "Unmouting folder"
		umount "$test_folder_name"
	fi

	rm -rf "$test_folder_name"
fi

mkfs.ext4 -F /dev/$disk_name"1"
if [ $? == 0 ]; then
	error "Created file system on a readonly disk"
fi

echo -e "d\n1\nw" | fdisk /dev/$disk_name
if [ $? == 0 ]; then
	error "Deleted partition from readonly disk!"
fi
