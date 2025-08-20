#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2016 Intel Corporation
#  All rights reserved.
#  Copyright (c) 2022 Dell Inc, or its subsidiaries.
#  Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
#

import sys
from spdk.rpc.client import print_dict, print_json, print_array  # noqa


def add_parser(subparsers):

    def iobuf_set_options(args):
        args.client.iobuf_set_options(
                                    small_pool_count=args.small_pool_count,
                                    large_pool_count=args.large_pool_count,
                                    small_bufsize=args.small_bufsize,
                                    large_bufsize=args.large_bufsize,
                                    enable_numa=args.enable_numa)
    p = subparsers.add_parser('iobuf_set_options', help='Set iobuf pool options')
    p.add_argument('--small-pool-count', help='number of small buffers in the global pool', type=int)
    p.add_argument('--large-pool-count', help='number of large buffers in the global pool', type=int)
    p.add_argument('--small-bufsize', help='size of a small buffer', type=int)
    p.add_argument('--large-bufsize', help='size of a large buffer', type=int)
    p.add_argument('--enable-numa', help='enable per-NUMA node buffer pools', action='store_true')
    p.set_defaults(func=iobuf_set_options)

    def iobuf_get_stats(args):
        print_dict(args.client.iobuf_get_stats())

    p = subparsers.add_parser('iobuf_get_stats', help='Display iobuf statistics')
    p.set_defaults(func=iobuf_get_stats)
