#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2016 Intel Corporation
#  All rights reserved.
#  Copyright (c) 2022 Dell Inc, or its subsidiaries.
#  Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
#

import argparse

from spdk.rpc.client import print_array, print_dict, print_json  # noqa
from spdk.rpc.helpers import DeprecateFalseAction, DeprecateTrueAction


def add_parser(subparsers):

    def fsdev_get_opts(args):
        print_json(args.client.fsdev_get_opts())

    p = subparsers.add_parser('fsdev_get_opts', help='Get the fsdev subsystem options')
    p.set_defaults(func=fsdev_get_opts)

    def fsdev_set_opts(args):
        print(args.client.fsdev_set_opts(fsdev_io_pool_size=args.fsdev_io_pool_size,
                                         fsdev_io_cache_size=args.fsdev_io_cache_size))

    p = subparsers.add_parser('fsdev_set_opts', help='Set the fsdev subsystem options')
    p.add_argument('fsdev_io_pool_size', help='Size of fsdev IO objects pool', type=int)
    p.add_argument('fsdev_io_cache_size', help='Size of fsdev IO objects cache per thread', type=int)
    p.set_defaults(func=fsdev_set_opts)

    def fsdev_aio_create(args):
        print(args.client.fsdev_aio_create(name=args.name, root_path=args.root_path,
                                           enable_xattr=args.enable_xattr, enable_writeback_cache=args.enable_writeback_cache,
                                           max_write=args.max_write, skip_rw=args.skip_rw))

    p = subparsers.add_parser('fsdev_aio_create', help='Create a aio filesystem')
    p.add_argument('name', help='Filesystem name. Example: aio0.')
    p.add_argument('root_path', help='Path on the system fs to expose as SPDK filesystem')

    # TODO: this group is deprecated, remove in next version
    group = p.add_mutually_exclusive_group()
    group.add_argument('--enable-xattr',  help='Enable extended attributes', dest='enable_xattr',
                       action=DeprecateTrueAction, default=None)
    group.add_argument('--disable-xattr', help='Disable extended attributes', dest='enable_xattr',
                       action=DeprecateFalseAction, default=None)
    group.add_argument('--xattr', dest='enable_xattr', action=argparse.BooleanOptionalAction,
                       help='Enable or disable extended attributes')

    # TODO: this group is deprecated, remove in next version
    group = p.add_mutually_exclusive_group()
    group.add_argument('--enable-writeback-cache',  help='Enable writeback cache', dest='enable_writeback_cache',
                       action=DeprecateTrueAction, default=None)
    group.add_argument('--disable-writeback-cache', help='Disable writeback cache', dest='enable_writeback_cache',
                       action=DeprecateFalseAction, default=None)
    group.add_argument('--writeback-cache', dest='enable_writeback_cache', action=argparse.BooleanOptionalAction,
                       help='Enable or disable writeback cache')

    p.add_argument('-w', '--max-write', help='Max write size in bytes', type=int)

    p.add_argument('--skip-rw', dest='skip_rw', help="Do not process read or write commands. This is used for testing.",
                   action='store_true', default=None)

    p.set_defaults(func=fsdev_aio_create)

    def fsdev_aio_delete(args):
        print(args.client.fsdev_aio_delete(name=args.name))

    p = subparsers.add_parser('fsdev_aio_delete', help='Delete a aio filesystem')
    p.add_argument('name', help='Filesystem name. Example: aio0.')
    p.set_defaults(func=fsdev_aio_delete)
