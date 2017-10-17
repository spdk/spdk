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

if (( $(lsblk -r -n -o RO -d "/dev/$disk_name") == 1 )); then
	error "Vhost-blk disk is set as readonly!"
fi

if cat /proc/partitions | grep -q1 $disk_name"1";then
	echo "Warning: Partition has been discovered. Removing partition."
	echo -e "d\n1\nw" | fdisk /dev/$disk_name
fi

echo "Creating new partition."
echo -e "n\np\n1\n\n\nw" | fdisk /dev/$disk_name

if [ $? != 0 ]; then
	error "Failed to create partition!"
fi

partprobe
sleep 0.1
cat /proc/partitions | grep -q1 $disk_name"1"
if [ $? != 0 ]; then
	error "Partition not found!"
fi

mkfs.ext4 -F /dev/$disk_name"1"
if [ $? != 0 ]; then
	error "Failed to create file system"
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
	error "Failed to mount disk"
fi

echo "Creating a test file $test_file_name"
dd if=/dev/zero of=./$test_folder_name/$test_file_name bs=1M count=200

echo "Unmouting partition"
if [ -d "$test_folder_name" ];then
	if mount | grep -q "$test_folder_name"; then
		echo "unmouting folder"
		umount "$test_folder_name"
	fi

	rm -rf "$test_folder_name"
fi
