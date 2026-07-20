#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2016 Intel Corporation
#  All rights reserved.
#  Copyright (c) 2022 Dell Inc, or its subsidiaries.
#  Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
#

from functools import partial

from spdk.rpc.cmd_parser import print_dict


def add_parser(subparsers):

    def trace_enable_tpoint_group(args):
        args.client.trace_enable_tpoint_group(name=args.name)

    p = subparsers.add_parser('trace_enable_tpoint_group',
                              help='Enable trace on a specific tpoint group. For example "bdev" for bdev'
                                   ' trace group, "all" for all trace groups.')
    p.add_argument(
        'name', help='Name of a registered trace group, or "all" to enable every group')
    p.set_defaults(func=trace_enable_tpoint_group)

    def trace_disable_tpoint_group(args):
        args.client.trace_disable_tpoint_group(name=args.name)

    p = subparsers.add_parser('trace_disable_tpoint_group',
                              help='Disable trace on a specific tpoint group. For example "bdev" for'
                                   ' bdev trace group, "all" for all trace groups.')
    p.add_argument(
        'name', help='Name of a registered trace group, or "all" to disable every group')
    p.set_defaults(func=trace_disable_tpoint_group)

    def trace_set_tpoint_mask(args):
        args.client.trace_set_tpoint_mask(name=args.name, tpoint_mask=args.tpoint_mask)

    p = subparsers.add_parser('trace_set_tpoint_mask',
                              help='Enable tracepoint mask on a specific tpoint group. For example "bdev"'
                                   " for bdev trace group, and 0x1 to enable the first tracepoint inside"
                                   " the group (BDEV_IO_START). This command will not disable already"
                                   " active tracepoints or those not specified in the mask. For a full"
                                   " description of all available trace groups, see"
                                   " [tracepoint documentation](https://spdk.io/doc/tracepoints.html).")
    p.add_argument(
        'name', help='Name of a registered trace group (e.g. "bdev", "nvmf_rdma", "nvmf_tcp")')
    p.add_argument(
        'tpoint_mask',
        help='Bitmask of tracepoints to enable within the group (e.g. 0x3 enables the first two)',
        type=partial(int, base=16))
    p.set_defaults(func=trace_set_tpoint_mask)

    def trace_clear_tpoint_mask(args):
        args.client.trace_clear_tpoint_mask(name=args.name, tpoint_mask=args.tpoint_mask)

    p = subparsers.add_parser('trace_clear_tpoint_mask',
                              help='Disable tracepoint mask on a specific tpoint group. For example "bdev"'
                                   " for bdev trace group, and 0x1 to disable the first tracepoint inside"
                                   " the group (BDEV_IO_START). For a full description of all available"
                                   " trace groups, see"
                                   " [tracepoint documentation](https://spdk.io/doc/tracepoints.html).")
    p.add_argument(
        'name', help='Name of a registered trace group (e.g. "bdev", "nvmf_rdma", "nvmf_tcp")')
    p.add_argument(
        'tpoint_mask',
        help='Bitmask of tracepoints to disable within the group (e.g. 0x3 disables the first two)',
        type=partial(int, base=16))
    p.set_defaults(func=trace_clear_tpoint_mask)

    def trace_get_tpoint_group_mask(args):
        print_dict(args.client.trace_get_tpoint_group_mask())

    p = subparsers.add_parser('trace_get_tpoint_group_mask', help="Display mask info for every group.")
    p.set_defaults(func=trace_get_tpoint_group_mask)

    def trace_clear(args):
        args.client.trace_clear()

    p = subparsers.add_parser('trace_clear',
                              help="Clear trace history. All entries recorded before this call will be"
                                   " hidden from subsequent reads by external trace tools such as"
                                   " spdk_trace. This does not disable tracing; new entries continue to"
                                   " be recorded.")
    p.set_defaults(func=trace_clear)

    def trace_get_info(args):
        print_dict(args.client.trace_get_info())

    p = subparsers.add_parser('trace_get_info',
                              help="Get name of shared memory file, list of the available trace point"
                                   " groups and mask of the available trace points for each group")
    p.set_defaults(func=trace_get_info)
