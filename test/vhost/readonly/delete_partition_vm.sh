#!/usr/bin/env bash

set -xe

testdir=$(readlink -f $(dirname $0))

disk_name="vda"
test_folder_name="readonly_test"
test_file_name="some_test_file"

function error() {
	echo "==========="
	echo -e "ERROR: $*"
	echo "==========="
	trap - ERR
	set +e
	umount "$test_folder_name"
	rm -rf "${testdir:?}/${test_folder_name:?}"
	exit 1
}

trap 'error "In delete_partition_vm.sh, line:" "${LINENO}"' ERR

if [[ ! -d "/sys/block/$disk_name" ]]; then
	error "No vhost-blk disk found!"
fi

if (($(lsblk -r -n -o RO -d "/dev/$disk_name") == 1)); then
	error "Vhost-blk disk is set as readonly!"
fi

mkdir -p $test_folder_name

echo "INFO: Mounting disk"
mount /dev/$disk_name"1" $test_folder_name

echo "INFO: Removing folder and unmounting $test_folder_name"
umount "$test_folder_name"
rm -rf "${testdir:?}/${test_folder_name:?}"

echo "INFO: Deleting partition"
# Zap the entire drive to make sure the partition table is removed as well
wipefs --all "/dev/$disk_name"
