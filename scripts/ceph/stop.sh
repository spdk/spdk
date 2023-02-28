#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2017 Intel Corporation
#  All rights reserved.
#
set -x

base_dir=/var/tmp/ceph
image=${base_dir}/ceph_raw.img
dev=/dev/loop200

pkill -9 ceph
sleep 3
umount ${dev}p2
losetup -d $dev
rm -rf $base_dir
