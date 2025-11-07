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

    def log_set_flag(args):
        args.client.log_set_flag(flag=args.flag)

    p = subparsers.add_parser('log_set_flag', help='set log flag')
    p.add_argument(
        'flag', help='log flag we want to set. (for example "nvme").')
    p.set_defaults(func=log_set_flag)

    def log_clear_flag(args):
        args.client.log_clear_flag(flag=args.flag)

    p = subparsers.add_parser('log_clear_flag', help='clear log flag')
    p.add_argument(
        'flag', help='log flag we want to clear. (for example "nvme").')
    p.set_defaults(func=log_clear_flag)

    def log_get_flags(args):
        print_dict(args.client.log_get_flags())

    p = subparsers.add_parser('log_get_flags', help='get log flags')
    p.set_defaults(func=log_get_flags)

    def log_set_level(args):
        args.client.log_set_level(level=args.level)

    p = subparsers.add_parser('log_set_level', help='set log level')
    p.add_argument('level', help='log level we want to set. (for example "DEBUG").')
    p.set_defaults(func=log_set_level)

    def log_get_level(args):
        print_dict(args.client.log_get_level())

    p = subparsers.add_parser('log_get_level', help='get log level')
    p.set_defaults(func=log_get_level)

    def log_set_print_level(args):
        args.client.log_set_print_level(level=args.level)

    p = subparsers.add_parser('log_set_print_level', help='set log print level')
    p.add_argument('level', help='log print level we want to set. (for example "DEBUG").')
    p.set_defaults(func=log_set_print_level)

    def log_get_print_level(args):
        print_dict(args.client.log_get_print_level())

    p = subparsers.add_parser('log_get_print_level', help='get log print level')
    p.set_defaults(func=log_get_print_level)

    def log_enable_timestamps(args):
        args.client.log_enable_timestamps(enabled=args.enabled)
    p = subparsers.add_parser('log_enable_timestamps',
                              help='Enable or disable timestamps.')
    # TODO: this group is deprecated, remove in next version
    group = p.add_mutually_exclusive_group(required=True)
    group.add_argument('-d', '--disable', dest='enabled', action=DeprecateFalseAction, help="Disable timestamps", default=False)
    group.add_argument('-e', '--enable',  dest='enabled', action=DeprecateTrueAction, help="Enable timestamps")
    group.add_argument('--timestamps', dest='enabled', action=argparse.BooleanOptionalAction, help='Enable or disable timestamps')
    p.set_defaults(func=log_enable_timestamps)
