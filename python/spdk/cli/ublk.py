#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2016 Intel Corporation
#  All rights reserved.
#  Copyright (c) 2022 Dell Inc, or its subsidiaries.
#  Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
#

import sys
from spdk.rpc.client import print_dict, print_json, print_array  # noqa


def add_parser(subparsers):

    def ublk_create_target(args):
        args.client.ublk_create_target(
                                    cpumask=args.cpumask,
                                    disable_user_copy=args.disable_user_copy)
    p = subparsers.add_parser('ublk_create_target',
                              help='Create spdk ublk target for ublk dev')
    p.add_argument('-m', '--cpumask', help='cpu mask for ublk dev')
    p.add_argument('--disable-user-copy', help='Disable user copy feature', action='store_true')
    p.set_defaults(func=ublk_create_target)

    def ublk_destroy_target(args):
        args.client.ublk_destroy_target()
    p = subparsers.add_parser('ublk_destroy_target',
                              help='Destroy spdk ublk target for ublk dev')
    p.set_defaults(func=ublk_destroy_target)

    def ublk_start_disk(args):
        print(args.client.ublk_start_disk(
                                       bdev_name=args.bdev_name,
                                       ublk_id=args.ublk_id,
                                       num_queues=args.num_queues,
                                       queue_depth=args.queue_depth))

    p = subparsers.add_parser('ublk_start_disk',
                              help='Export a bdev as a ublk device')
    p.add_argument('bdev_name', help='Blockdev name to be exported. Example: Malloc0.')
    p.add_argument('ublk_id', help='ublk device id to be assigned. Example: 1.', type=int)
    p.add_argument('-q', '--num-queues', help="the total number of queues. Example: 1", type=int)
    p.add_argument('-d', '--queue-depth', help="queue depth. Example: 128", type=int)
    p.set_defaults(func=ublk_start_disk)

    def ublk_stop_disk(args):
        args.client.ublk_stop_disk(ublk_id=args.ublk_id)

    p = subparsers.add_parser('ublk_stop_disk',
                              help='Stop a ublk device')
    p.add_argument('ublk_id', help='ublk device id to be deleted. Example: 1.', type=int)
    p.set_defaults(func=ublk_stop_disk)

    def ublk_recover_disk(args):
        print(args.client.ublk_recover_disk(
                                         bdev_name=args.bdev_name,
                                         ublk_id=args.ublk_id))

    p = subparsers.add_parser('ublk_recover_disk',
                              help='Recover ublk device')
    p.add_argument('bdev_name', help='Blockdev name to be recovered. Example: Malloc0.')
    p.add_argument('ublk_id', help='ublk device id to be recovered. Example: 1.', type=int)
    p.set_defaults(func=ublk_recover_disk)

    def ublk_get_disks(args):
        print_dict(args.client.ublk_get_disks(ublk_id=args.ublk_id))

    p = subparsers.add_parser('ublk_get_disks',
                              help='Display full or specified ublk device list')
    p.add_argument('-n', '--ublk-id', help="ublk device id. Example: 1", type=int)
    p.set_defaults(func=ublk_get_disks)
