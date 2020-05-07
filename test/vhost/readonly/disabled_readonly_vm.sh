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

trap 'error "In disabled_readonly_vm.sh, line:" "${LINENO}"' ERR

if [[ ! -d "/sys/block/$disk_name" ]]; then
	error "No vhost-blk disk found!"
fi

if (($(lsblk -r -n -o RO -d "/dev/$disk_name") == 1)); then
	error "Vhost-blk disk is set as readonly!"
fi

parted -s /dev/$disk_name mklabel gpt
parted -s /dev/$disk_name mkpart primary 2048s 100%
partprobe
sleep 0.1

echo "INFO: Creating file system"
mkfs.ext4 -F /dev/$disk_name"1"

echo "INFO: Mounting disk"
mkdir -p $test_folder_name
mount /dev/$disk_name"1" $test_folder_name

echo "INFO: Creating a test file $test_file_name"
truncate -s "200M" $test_folder_name/$test_file_name
umount "$test_folder_name"
rm -rf "${testdir:?}/${test_folder_name:?}"
