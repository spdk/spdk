#!/usr/bin/env bash

set -x

base_dir=/var/tmp/ceph
dev=/dev/loop200

pkill -9 ceph
sleep 3
umount ${dev}p2
losetup -d $dev
rm -rf $base_dir
