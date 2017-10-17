#!/usr/bin/env bash
set -x

vhost_name=$(shopt -s nullglob; cd /sys/block; echo vd*)
readonly_folder="readonly_test"

if [ -z $vhost_name ]; then
	echo "no vhost disk found"
fi

if (( $(lsblk -r -n -o RO -d "/dev/$vhost_name") == 1 )); then
	echo "vhost disk is readonly"
fi

if cat /proc/partitions | grep -q1 $vhost_name"1";then
	echo "partition found"
	echo -e "d\n1\nw" | fdisk /dev/$vhost_name
fi

echo -e "n\np\n1\n\n\nw" | fdisk /dev/$vhost_name
partprobe
if cat /proc/partitions | grep -q1 $vhost_name"1";then
	echo "partition found"
fi

mkfs.ext4 -F /dev/$vhost_name"1"
if [ $? != 0 ]; then
	echo "Failed"
fi

if [ -d "$readonly_folder" ];then
	echo "Found folder"
	if mount | grep -q "$readonly_folder"; then
		echo "unmouting folder"
		umount "$readonly_folder"
	fi

	echo "Removing folder"
	rm -rf "$readonly_folder"
fi

mkdir $readonly_folder
if [ $? != 0 ]; then
	echo "Failed"
fi

mount /dev/$vhost_name"1" $readonly_folder
if [ $? != 0 ]; then
	echo "Failed"
fi

dd if=/dev/zero of=./$readonly_folder/ubuntu-base bs=1M count=200

if [ -d "$readonly_folder" ];then
	echo "Found folder"
	if mount | grep -q "$readonly_folder"; then
		echo "unmouting folder"
		umount "$readonly_folder"
	fi

	echo "Removing folder"
	rm -rf "$readonly_folder"
fi

echo "$vhost_name"
