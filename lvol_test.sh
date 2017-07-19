#!/bin/bash

ROOT_DIR="$(dirname $(readlink -f $0))"
#exit 1
rpc="${ROOT_DIR}/scripts/rpc.py"

set -x

if [ "$1" = "basic" ]; then
	MALLOC=`$rpc construct_malloc_bdev 128 4096`
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
#	$rpc delete_bdev $BLOB
#	$rpc destroy_lvol_store $GUID
#	$rpc get_bdevs

elif [ "$1" = "malloc" ]; then

	$rpc -v bdev_open Malloc0
	$rpc -v bdev_write 10
	$rpc -v bdev_read 10
	$rpc -v bdev_close Malloc0

elif [ "$1" = "prep" ]; then

	MALLOC=`$rpc construct_malloc_bdev 128 4096`
	GUID=`$rpc construct_lvol_store $MALLOC`
	BLOB=`$rpc construct_lvol_bdev $GUID 25`
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
	$rpc delete_bdev $BLOB
	$rpc destroy_lvol_store $GUID
	$rpc get_bdevs

elif [ "$1" = "delete" ]; then

GUID=`$rpc construct_lvol_store Malloc0`
BLOB=`$rpc construct_lvol_bdev $GUID 15`
BLOB2=`$rpc construct_lvol_bdev $GUID 15`
BLOB3=`$rpc construct_lvol_bdev $GUID 15`
$rpc get_bdevs
$rpc delete_bdev $BLOB_2
$rpc destroy_lvol_store $GUID
$rpc get_bdevs

elif [ "$1" = "nest" ]; then

GUID=`$rpc construct_lvol_store Malloc0`
BLOB=`$rpc construct_lvol_bdev $GUID 25`
GUID_2=`$rpc construct_lvol_store $BLOB`
BLOB_2=`$rpc construct_lvol_bdev $GUID_2 15`
$rpc get_bdevs
$rpc destroy_lvol_store $GUID

elif [ "$1" = "vhost_normal" ]; then

$rpc get_bdevs
$rpc add_vhost_scsi_lun vhost.0 0 Malloc0
$rpc get_vhost_scsi_controllers

elif [ "$1" = "vhost" ]; then

MALLOC=`$rpc construct_malloc_bdev 128 4096`
GUID=`$rpc construct_lvol_store Malloc0`
BLOB=`$rpc construct_lvol_bdev $GUID 25`
$rpc get_bdevs
$rpc add_vhost_scsi_lun vhost.0 0 $BLOB
$rpc get_vhost_scsi_controllers
fi
