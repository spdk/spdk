#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2016 Intel Corporation
#  All rights reserved.
#  Copyright (c) 2022 Dell Inc, or its subsidiaries.
#  Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
#

from spdk.rpc.cmd_parser import print_dict


def add_parser(subparsers):

    def iobuf_set_options(args):
        args.client.iobuf_set_options(
                                    small_pool_count=args.small_pool_count,
                                    large_pool_count=args.large_pool_count,
                                    small_bufsize=args.small_bufsize,
                                    large_bufsize=args.large_bufsize,
                                    enable_numa=args.enable_numa)
    p = subparsers.add_parser('iobuf_set_options', help='Set iobuf pool options')
    p.add_argument('--small-pool-count', help='Number of small buffers in the global pool. Default: 8192', type=int)
    p.add_argument('--large-pool-count', help='Number of large buffers in the global pool. Default: 1024', type=int)
    p.add_argument('--small-bufsize', help='Size of a small buffer in bytes. Default: 8192', type=int)
    p.add_argument('--large-bufsize', help='Size of a large buffer in bytes. Default: 135168', type=int)
    p.add_argument('--enable-numa',
                   help='Enable per-NUMA node buffer pools, each sized by small_pool_count and large_pool_count. Default: false',
                   action='store_true')
    p.set_defaults(func=iobuf_set_options)

    def iobuf_get_stats(args):
        print_dict(args.client.iobuf_get_stats())

    p = subparsers.add_parser('iobuf_get_stats', help='Display iobuf statistics')
    p.set_defaults(func=iobuf_get_stats)
