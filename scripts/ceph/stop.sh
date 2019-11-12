#!/usr/bin/env bash

set -x

base_dir=/var/tmp/ceph
image=${base_dir}/ceph_raw.img
dev=/dev/loop200

pkill -9 ceph
sleep 3
umount ${dev}p2
losetup -d $dev
rm -rf $base_dir
