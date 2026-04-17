#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2016 Intel Corporation
#  All rights reserved.
#  Copyright (c) 2022 Dell Inc, or its subsidiaries.
#  Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
#

from spdk.rpc.cmd_parser import print_dict


def add_parser(subparsers):

    def nbd_start_disk(args):
        print(args.client.nbd_start_disk(
                                     bdev_name=args.bdev_name,
                                     nbd_device=args.nbd_device))

    p = subparsers.add_parser('nbd_start_disk',
                              help='Export a bdev as an nbd disk')
    p.add_argument('bdev_name', help='Name of the bdev to export')
    p.add_argument('nbd_device', help='Path to the NBD device, e.g. "/dev/nbd0". Default: first available device', nargs='?')
    p.set_defaults(func=nbd_start_disk)

    def nbd_stop_disk(args):
        args.client.nbd_stop_disk(nbd_device=args.nbd_device)

    p = subparsers.add_parser('nbd_stop_disk',
                              help='Stop an nbd disk')
    p.add_argument('nbd_device', help='Path to the NBD device, e.g. "/dev/nbd0"')
    p.set_defaults(func=nbd_stop_disk)

    def nbd_get_disks(args):
        print_dict(args.client.nbd_get_disks(nbd_device=args.nbd_device))

    p = subparsers.add_parser('nbd_get_disks',
                              help='Display full or specified nbd device list')
    p.add_argument('-n', '--nbd-device', help='Path to the NBD device, e.g. "/dev/nbd0". Default: list all')
    p.set_defaults(func=nbd_get_disks)
