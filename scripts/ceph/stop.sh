#!/usr/bin/env bash

set -x

base_dir=`pwd`
home_folder=${base_dir}/ceph
image=${base_dir}/../../../build/ceph_raw.img
pkill -9 ceph
sleep 3
umount /dev/loop200p2
rm -rf $home_folder
rm $image
