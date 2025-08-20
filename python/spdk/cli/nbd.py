#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2016 Intel Corporation
#  All rights reserved.
#  Copyright (c) 2022 Dell Inc, or its subsidiaries.
#  Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
#

import sys
from spdk.rpc.client import print_dict, print_json, print_array  # noqa


def add_parser(subparsers):

    def nbd_start_disk(args):
        print(args.client.nbd_start_disk(
                                     bdev_name=args.bdev_name,
                                     nbd_device=args.nbd_device))

    p = subparsers.add_parser('nbd_start_disk',
                              help='Export a bdev as an nbd disk')
    p.add_argument('bdev_name', help='Blockdev name to be exported. Example: Malloc0.')
    p.add_argument('nbd_device', help='Nbd device name to be assigned. Example: /dev/nbd0.', nargs='?')
    p.set_defaults(func=nbd_start_disk)

    def nbd_stop_disk(args):
        args.client.nbd_stop_disk(nbd_device=args.nbd_device)

    p = subparsers.add_parser('nbd_stop_disk',
                              help='Stop an nbd disk')
    p.add_argument('nbd_device', help='Nbd device name to be stopped. Example: /dev/nbd0.')
    p.set_defaults(func=nbd_stop_disk)

    def nbd_get_disks(args):
        print_dict(args.client.nbd_get_disks(nbd_device=args.nbd_device))

    p = subparsers.add_parser('nbd_get_disks',
                              help='Display full or specified nbd device list')
    p.add_argument('-n', '--nbd-device', help="Path of the nbd device. Example: /dev/nbd0")
    p.set_defaults(func=nbd_get_disks)
