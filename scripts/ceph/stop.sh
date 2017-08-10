#!/usr/bin/env bash

set -x

base_dir=`pwd`
home_folder=${base_dir}/ceph
image=/var/tmp/ceph_raw.img
dev_backend=/dev/ceph

pkill -9 ceph
sleep 3
umount /dev/loop200p2
losetup -d $dev_backend
rm -rf $home_folder
rm $image
