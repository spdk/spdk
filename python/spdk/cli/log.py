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

    def log_set_flag(args):
        rpc.log.log_set_flag(args.client, flag=args.flag)

    p = subparsers.add_parser('log_set_flag', help='set log flag')
    p.add_argument(
        'flag', help='log flag we want to set. (for example "nvme").')
    p.set_defaults(func=log_set_flag)

    def log_clear_flag(args):
        rpc.log.log_clear_flag(args.client, flag=args.flag)

    p = subparsers.add_parser('log_clear_flag', help='clear log flag')
    p.add_argument(
        'flag', help='log flag we want to clear. (for example "nvme").')
    p.set_defaults(func=log_clear_flag)

    def log_get_flags(args):
        print_dict(rpc.log.log_get_flags(args.client))

    p = subparsers.add_parser('log_get_flags', help='get log flags')
    p.set_defaults(func=log_get_flags)

    def log_set_level(args):
        rpc.log.log_set_level(args.client, level=args.level)

    p = subparsers.add_parser('log_set_level', help='set log level')
    p.add_argument('level', help='log level we want to set. (for example "DEBUG").')
    p.set_defaults(func=log_set_level)

    def log_get_level(args):
        print_dict(rpc.log.log_get_level(args.client))

    p = subparsers.add_parser('log_get_level', help='get log level')
    p.set_defaults(func=log_get_level)

    def log_set_print_level(args):
        rpc.log.log_set_print_level(args.client, level=args.level)

    p = subparsers.add_parser('log_set_print_level', help='set log print level')
    p.add_argument('level', help='log print level we want to set. (for example "DEBUG").')
    p.set_defaults(func=log_set_print_level)

    def log_get_print_level(args):
        print_dict(rpc.log.log_get_print_level(args.client))

    p = subparsers.add_parser('log_get_print_level', help='get log print level')
    p.set_defaults(func=log_get_print_level)
