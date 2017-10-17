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

trap 'error "In disabled_readonly_vm.sh, line:" "${LINENO}"' ERR

cd $BASE_DIR

if ! $(cd /sys/block; ls | grep -qi "$disk_name$"); then
	error "No vhost-blk disk found!"
fi

if (( $(lsblk -r -n -o RO -d "/dev/$disk_name") == 1 )); then
	error "Vhost-blk disk is set as readonly!"
fi

parted -s /dev/$disk_name mklabel gpt
parted -s /dev/$disk_name mkpart primary 2048s 100%
partprobe
sleep 0.1

if [[ ! -b "/dev/$disk_name"1"" ]]; then
	error "Partition not found!"
fi

echo "INFO: Creating file system"
mkfs.ext4 -F /dev/$disk_name"1"

echo "INFO: Mounting disk"
mkdir -p $test_folder_name
mount /dev/$disk_name"1" $test_folder_name

echo "INFO: Creating a test file $test_file_name"
truncate -s "200M" $test_folder_name/$test_file_name
remove_test_folder
