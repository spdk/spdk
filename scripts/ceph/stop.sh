#!/usr/bin/env bash

set -x

base_dir=/var/tmp/ceph
image=${base_dir}/ceph_raw.img
dev_backend=/dev/ceph

pkill -9 ceph
sleep 3
umount /dev/loop200p2
losetup -d $dev_backend
rm -rf $base_dir
