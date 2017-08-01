#!/bin/bash

ROOT_DIR="$(dirname $(readlink -f $0))"
#exit 1
rpc="${ROOT_DIR}/scripts/rpc.py"

set -x

CTRL_NAME="vhost.0"
MALLOC=`$rpc construct_malloc_bdev 128 4096`

if [ "$1" = "basic" ]; then
	GUID=`$rpc construct_lvol_store $MALLOC`
	BLOB=`$rpc construct_lvol_bdev $GUID 25`
#	BLOB_2=`$rpc construct_lvol_bdev $GUID 25`
#	$rpc resize_lvol_bdev $BLOB 30
#	$rpc resize_lvol_bdev $BLOB_2 25
	$rpc -v bdev_open $BLOB
	$rpc -v bdev_write 1000
	$rpc -v bdev_read 50
	$rpc -v bdev_read 512
	$rpc -v bdev_read 128
	$rpc -v bdev_write 100
	$rpc -v bdev_close $BLOB

#	$rpc delete_bdev $BLOB_2
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

elif [ "$1" = "delete" ]; then

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

GUID=`$rpc construct_lvol_store $MALLOC`
$rpc get_bdevs

elif [ "$1" = "just_malloc" ]; then

GUID=`$rpc construct_lvol_store $MALLOC`
BLOB=`$rpc construct_lvol_bdev $GUID 15`
$rpc get_bdevs

elif [ "$1" = "nest" ]; then

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

$rpc get_bdevs
$rpc add_vhost_scsi_lun vhost.0 0 $MALLOC
$rpc get_vhost_scsi_controllers

elif [ "$1" = "vhost-scsi" ]; then

GUID=`$rpc construct_lvol_store $MALLOC`
BLOB=`$rpc construct_lvol_bdev $GUID 25`
$rpc get_bdevs
$rpc construct_vhost_scsi_controller $CTRL_NAME
$rpc add_vhost_scsi_lun $CTRL_NAME 0 $BLOB
$rpc get_vhost_scsi_controllers

elif [ "$1" = "vhost-blk" ]; then

GUID=`$rpc construct_lvol_store $MALLOC`
BLOB=`$rpc construct_lvol_bdev $GUID 25`
$rpc get_bdevs
$rpc construct_vhost_blk_controller $CTRL_NAME $BLOB
$rpc get_vhost_blk_controllers
fi
