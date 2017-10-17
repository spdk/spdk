#!/usr/bin/env bash

set -e
BASE_DIR=$(readlink -f $(dirname $0))

disk_name="vda"
test_folder_name="readonly_test"
test_file_name="some_test_file"

function remove_test_folder()
{
	if mount | grep -q "$test_folder_name"; then
		echo "INFO: Unmouting folder $test_folder_name"
		umount "$test_folder_name"
	fi

	cd $BASE_DIR
	rm -rf "$test_folder_name"
}

function error()
{
	echo "==========="
	echo -e "ERROR: $@"
	echo "==========="
	trap - ERR
	set +e
	remove_test_folder
	exit 1
}

trap 'error "In delete_partition_vm.sh, line:" "${LINENO}"' ERR

cd $BASE_DIR

if ! $(cd /sys/block; ls | grep -qi $disk_name$); then
	error "No vhost-blk disk found!"
fi

if (( $(lsblk -r -n -o RO -d "/dev/$disk_name") == 1 )); then
	error "Vhost-blk disk is set as readonly!"
fi

mkdir -p $test_folder_name

echo "INFO: Mounting disk"
mount /dev/$disk_name"1" $test_folder_name

if [ -f $test_folder_name/$test_file_name ]; then
	echo "INFO: Removing previously created file"
	rm $test_folder_name/$test_file_name
else
	echo "INFO: Previously created file not found"
fi

echo "INFO: Removing folder and unmounting $test_folder_name"
remove_test_folder

echo "INFO: Deleting partition"
echo -e "d\n1\nw" | fdisk /dev/$disk_name
