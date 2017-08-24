#!/bin/bash

ROOT_DIR="$(dirname $(readlink -f $0))"
#exit 1
rpc="${ROOT_DIR}/scripts/rpc.py"

set -xe

CTRL_NAME="vhost.0"

if [ "$1" = "vec_multi_dev" ]; then
	for i in {1..1000}
	do
		MALLOC=`$rpc construct_malloc_bdev 128 4096`
		GUID=`$rpc construct_lvol_store $MALLOC`
		BLOB=`$rpc construct_lvol_bdev $GUID 25`
		$rpc bdev_open $BLOB
		CHAR=$(cat /dev/urandom | tr -dc 'a-zA-Z0-9' | fold -w 1 | head -n 1)
		$rpc bdev_writev $((($i % 4)+1)) $((($i % 25)+1)) $CHAR
		$rpc bdev_readv $((($i % 4)+1)) $((($i % 25)+1)) $CHAR
		$rpc bdev_close $BLOB
		$rpc delete_bdev $BLOB
		$rpc destroy_lvol_store $GUID
		$rpc delete_bdev $MALLOC
	done
	$rpc get_bdevs

elif [ "$1" = "vec_single_dev" ]; then
	MALLOC=`$rpc construct_malloc_bdev 128 4096`
	GUID=`$rpc construct_lvol_store $MALLOC`
	BLOB=`$rpc construct_lvol_bdev $GUID 25`
	$rpc bdev_open $BLOB
	for i in {1..1000}
	do
		CHAR=$(cat /dev/urandom | tr -dc 'a-zA-Z0-9' | fold -w 1 | head -n 1)
		$rpc bdev_writev $((($i % 4)+1)) $((($i % 25)+1)) $CHAR
		$rpc bdev_readv $((($i % 4)+1)) $((($i % 25)+1)) $CHAR
	done
	$rpc bdev_close $BLOB
	$rpc delete_bdev $BLOB
	$rpc destroy_lvol_store $GUID
	$rpc delete_bdev $MALLOC
	$rpc get_bdevs

elif [ "$1" = "malloc" ]; then

	$rpc -v bdev_open $MALLOC
	$rpc -v bdev_write 10
	$rpc -v bdev_read 10
	$rpc -v bdev_close $MALLOC

elif [ "$1" = "prep" ]; then

	GUID=`$rpc construct_lvol_store $MALLOC`
#	BLOB=`$rpc construct_lvol_bdev $GUID 25`
#	BLOB_2=`$rpc construct_lvol_bdev $GUID 25`
	$rpc get_bdevs
#	$rpc resize_lvol_bdev $BLOB 30
#	$rpc resize_lvol_bdev $BLOB_2 25
#	$rpc -v bdev_open $BLOB
#	$rpc -v bdev_write 10
#	$rpc -v bdev_read 10
#	$rpc -v bdev_read 512
#	$rpc -v bdev_read 128
#	$rpc -v bdev_write 100
#	$rpc -v bdev_close $BLOB

#	$rpc delete_bdev $BLOB_2
#	$rpc delete_bdev $BLOB
	$rpc destroy_lvol_store $GUID
#	$rpc delete_bdev $MALLOC
	$rpc get_bdevs

elif [ "$1" = "resize" ]; then

	MALLOC=`$rpc construct_malloc_bdev 128 4096`
	GUID=`$rpc construct_lvol_store $MALLOC`
	BLOB=`$rpc construct_lvol_bdev $GUID 25`
	$rpc resize_lvol_bdev $BLOB 30
	$rpc resize_lvol_bdev $BLOB 15
	$rpc resize_lvol_bdev $BLOB 127
	$rpc resize_lvol_bdev $BLOB 128

	$rpc delete_bdev $BLOB
	$rpc destroy_lvol_store $GUID
	$rpc delete_bdev $MALLOC
	$rpc get_bdevs

elif [ "$1" = "copy" ]; then

#MALLOC=`$rpc construct_malloc_bdev 128 4096`
GUID=`$rpc construct_lvol_store Nvme0n1`
#BLOB=`$rpc construct_lvol_bdev $GUID 25`

#$rpc destroy_lvol_store $GUID
#GUID=`$rpc construct_lvol_store $MALLOC`

#BLOB=`$rpc construct_lvol_bdev $GUID 15`

#$rpc delete_bdev $BLOB
#BLOB=`$rpc construct_lvol_bdev $GUID 15`

#$rpc delete_bdev $BLOB
#$rpc destroy_lvol_store $GUID
#$rpc delete_bdev $MALLOC
$rpc get_bdevs

elif [ "$1" = "delete" ]; then

MALLOC=`$rpc construct_malloc_bdev 128 4096`
GUID=`$rpc construct_lvol_store $MALLOC`
BLOB=`$rpc construct_lvol_bdev $GUID 15`
BLOB2=`$rpc construct_lvol_bdev $GUID 15`
BLOB3=`$rpc construct_lvol_bdev $GUID 15`
$rpc get_bdevs
$rpc delete_bdev $BLOB2
$rpc destroy_lvol_store $GUID
$rpc delete_bdev $MALLOC
$rpc get_bdevs

elif [ "$1" = "store" ]; then

#GUID=`$rpc construct_lvol_store $MALLOC`
BLOB=`$rpc construct_lvol_bdev de07875c-7b4f-11e7-b84b-0cc47aa4ba38 15`
$rpc get_bdevs

elif [ "$1" = "just_malloc" ]; then

GUID=`$rpc construct_lvol_store $MALLOC`
BLOB=`$rpc construct_lvol_bdev $GUID 15`
$rpc get_bdevs

elif [ "$1" = "nest" ]; then

MALLOC=`$rpc construct_malloc_bdev 128 4096`
GUID=`$rpc construct_lvol_store $MALLOC`
BLOB=`$rpc construct_lvol_bdev $GUID 25`
$rpc get_bdevs
GUID_2=`$rpc construct_lvol_store $BLOB`
$rpc get_bdevs
BLOB_2=`$rpc construct_lvol_bdev $GUID_2 15`
$rpc get_bdevs
#$rpc destroy_lvol_store $GUID_2
$rpc destroy_lvol_store $GUID
$rpc delete_bdev $MALLOC
$rpc get_bdevs

elif [ "$1" = "vhost_normal" ]; then

MALLOC=`$rpc construct_malloc_bdev 128 4096`
$rpc construct_vhost_scsi_controller $CTRL_NAME
$rpc add_vhost_scsi_lun $CTRL_NAME 0 $MALLOC
$rpc get_vhost_scsi_controllers

elif [ "$1" = "vhost-scsi" ]; then

MALLOC=`$rpc construct_malloc_bdev 128 4096`
GUID=`$rpc construct_lvol_store $MALLOC`
BLOB=`$rpc construct_lvol_bdev $GUID 25`
$rpc get_bdevs
$rpc construct_vhost_scsi_controller $CTRL_NAME
$rpc add_vhost_scsi_lun $CTRL_NAME 0 $BLOB
$rpc get_vhost_scsi_controllers

elif [ "$1" = "vhost-scsi_nvme" ]; then

NVME="Nvme0n1"
GUID=`$rpc construct_lvol_store $NVME`
BLOB=`$rpc construct_lvol_bdev $GUID 1000`
$rpc get_bdevs
$rpc construct_vhost_scsi_controller $CTRL_NAME
$rpc add_vhost_scsi_lun $CTRL_NAME 0 $BLOB
$rpc get_vhost_controllers

elif [ "$1" = "vhost-blk" ]; then

NVME="Nvme0n1"
GUID=`$rpc construct_lvol_store $NVME`

CTRL_NAME="vhost.0"
BLOB=`$rpc construct_lvol_bdev $GUID 1000`
$rpc construct_vhost_blk_controller $CTRL_NAME $BLOB

CTRL_NAME="vhost.1"
BLOB=`$rpc construct_lvol_bdev $GUID 1000`
$rpc construct_vhost_blk_controller $CTRL_NAME $BLOB

CTRL_NAME="vhost.2"
BLOB=`$rpc construct_lvol_bdev $GUID 1000`
$rpc construct_vhost_blk_controller $CTRL_NAME $BLOB

CTRL_NAME="vhost.3"
BLOB=`$rpc construct_lvol_bdev $GUID 1000`
$rpc construct_vhost_blk_controller $CTRL_NAME $BLOB

$rpc get_vhost_controllers
fi
