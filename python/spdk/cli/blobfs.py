#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2016 Intel Corporation
#  All rights reserved.
#  Copyright (c) 2022 Dell Inc, or its subsidiaries.
#  Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
#

import sys
import spdk.rpc as rpc  # noqa
from spdk.rpc.client import print_dict, print_json, print_array  # noqa


def add_parser(subparsers):

    def blobfs_detect(args):
        print("blobfs_detect RPC is deprecated", file=sys.stderr)
        print(rpc.blobfs.blobfs_detect(args.client,
                                       bdev_name=args.bdev_name))

    p = subparsers.add_parser('blobfs_detect', help='Detect whether a blobfs exists on bdev')
    p.add_argument('bdev_name', help='Blockdev name to detect blobfs. Example: Malloc0.')
    p.set_defaults(func=blobfs_detect)

    def blobfs_create(args):
        print("blobfs_create RPC is deprecated", file=sys.stderr)
        print(rpc.blobfs.blobfs_create(args.client,
                                       bdev_name=args.bdev_name,
                                       cluster_sz=args.cluster_sz))

    p = subparsers.add_parser('blobfs_create', help='Build a blobfs on bdev')
    p.add_argument('bdev_name', help='Blockdev name to build blobfs. Example: Malloc0.')
    p.add_argument('-c', '--cluster-sz',
                   help="""Size of cluster in bytes (Optional). Must be multiple of 4KB page size. Default and minimal value is 1M.""")
    p.set_defaults(func=blobfs_create)

    def blobfs_mount(args):
        print("blobfs_mount RPC is deprecated", file=sys.stderr)
        print(rpc.blobfs.blobfs_mount(args.client,
                                      bdev_name=args.bdev_name,
                                      mountpoint=args.mountpoint))

    p = subparsers.add_parser('blobfs_mount', help='Mount a blobfs on bdev to host path by FUSE')
    p.add_argument('bdev_name', help='Blockdev name where the blobfs is. Example: Malloc0.')
    p.add_argument('mountpoint', help='Mountpoint path in host to mount blobfs. Example: /mnt/.')
    p.set_defaults(func=blobfs_mount)

    def blobfs_set_cache_size(args):
        print("blobfs_set_cache_size RPC is deprecated", file=sys.stderr)
        print(rpc.blobfs.blobfs_set_cache_size(args.client,
                                               size_in_mb=args.size_in_mb))

    p = subparsers.add_parser('blobfs_set_cache_size', help='Set cache size for blobfs')
    p.add_argument('size_in_mb', help='Cache size for blobfs in megabytes.', type=int)
    p.set_defaults(func=blobfs_set_cache_size)
