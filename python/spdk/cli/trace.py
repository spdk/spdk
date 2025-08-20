#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2016 Intel Corporation
#  All rights reserved.
#  Copyright (c) 2022 Dell Inc, or its subsidiaries.
#  Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
#

import sys
from spdk.rpc.client import print_dict, print_json, print_array  # noqa


def add_parser(subparsers):

    def trace_enable_tpoint_group(args):
        args.client.trace_enable_tpoint_group(name=args.name)

    p = subparsers.add_parser('trace_enable_tpoint_group',
                              help='enable trace on a specific tpoint group')
    p.add_argument(
        'name', help="""trace group name we want to enable in tpoint_group_mask.
        (for example "bdev" for bdev trace group, "all" for all trace groups).""")
    p.set_defaults(func=trace_enable_tpoint_group)

    def trace_disable_tpoint_group(args):
        args.client.trace_disable_tpoint_group(name=args.name)

    p = subparsers.add_parser('trace_disable_tpoint_group',
                              help='disable trace on a specific tpoint group')
    p.add_argument(
        'name', help="""trace group name we want to disable in tpoint_group_mask.
        (for example "bdev" for bdev trace group, "all" for all trace groups).""")
    p.set_defaults(func=trace_disable_tpoint_group)

    def trace_set_tpoint_mask(args):
        args.client.trace_set_tpoint_mask(name=args.name, tpoint_mask=args.tpoint_mask)

    p = subparsers.add_parser('trace_set_tpoint_mask',
                              help='enable tracepoint mask on a specific tpoint group')
    p.add_argument(
        'name', help="""trace group name we want to enable in tpoint_group_mask.
        (for example "bdev" for bdev trace group)""")
    p.add_argument(
        'tpoint_mask', help="""tracepoints to be enabled inside a given trace group.
        (for example value of "0x3" will enable only the first two tpoints in this group)""",
        type=lambda m: int(m, 16))
    p.set_defaults(func=trace_set_tpoint_mask)

    def trace_clear_tpoint_mask(args):
        args.client.trace_clear_tpoint_mask(name=args.name, tpoint_mask=args.tpoint_mask)

    p = subparsers.add_parser('trace_clear_tpoint_mask',
                              help='disable tracepoint mask on a specific tpoint group')
    p.add_argument(
        'name', help="""trace group name we want to disable in tpoint_group_mask.
        (for example "bdev" for bdev trace group)""")
    p.add_argument(
        'tpoint_mask', help="""tracepoints to be disabled inside a given trace group.
        (for example value of "0x3" will disable the first two tpoints in this group)""",
        type=lambda m: int(m, 16))
    p.set_defaults(func=trace_clear_tpoint_mask)

    def trace_get_tpoint_group_mask(args):
        print_dict(args.client.trace_get_tpoint_group_mask())

    p = subparsers.add_parser('trace_get_tpoint_group_mask', help='get trace point group mask')
    p.set_defaults(func=trace_get_tpoint_group_mask)

    def trace_get_info(args):
        print_dict(args.client.trace_get_info())

    p = subparsers.add_parser('trace_get_info',
                              help='get name of shared memory file and list of the available trace point groups')
    p.set_defaults(func=trace_get_info)
