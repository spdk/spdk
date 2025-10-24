#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2016 Intel Corporation
#  All rights reserved.
#  Copyright (c) 2022 Dell Inc, or its subsidiaries.
#  Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
#

import sys
from spdk.rpc.client import print_dict, print_json, print_array  # noqa


def add_parser(subparsers):

    def sock_impl_get_options(args):
        print_json(args.client.sock_impl_get_options(impl_name=args.impl))

    p = subparsers.add_parser('sock_impl_get_options', help="""Get options of socket layer implementation""")
    p.add_argument('-i', '--impl', help='Socket implementation name, e.g. posix', required=True)
    p.set_defaults(func=sock_impl_get_options)

    def sock_impl_set_options(args):
        args.client.sock_impl_set_options(
                                       impl_name=args.impl,
                                       recv_buf_size=args.recv_buf_size,
                                       send_buf_size=args.send_buf_size,
                                       enable_recv_pipe=args.enable_recv_pipe,
                                       enable_quickack=args.enable_quickack,
                                       enable_placement_id=args.enable_placement_id,
                                       enable_zerocopy_send_server=args.enable_zerocopy_send_server,
                                       enable_zerocopy_send_client=args.enable_zerocopy_send_client,
                                       zerocopy_threshold=args.zerocopy_threshold,
                                       tls_version=args.tls_version,
                                       enable_ktls=args.enable_ktls)

    p = subparsers.add_parser('sock_impl_set_options', help="""Set options of socket layer implementation""")
    p.add_argument('-i', '--impl', help='Socket implementation name, e.g. posix', required=True)
    p.add_argument('-r', '--recv-buf-size', help='Size of receive buffer on socket in bytes', type=int)
    p.add_argument('-s', '--send-buf-size', help='Size of send buffer on socket in bytes', type=int)
    p.add_argument('-p', '--enable-placement-id', help='Option for placement-id. 0:disable,1:incoming_napi,2:incoming_cpu', type=int)
    group = p.add_mutually_exclusive_group()
    group.add_argument('--enable-recv-pipe', help='Enable receive pipe',
                       action='store_true', dest='enable_recv_pipe')
    group.add_argument('--disable-recv-pipe', help='Disable receive pipe',
                       action='store_false', dest='enable_recv_pipe')
    group = p.add_mutually_exclusive_group()
    group.add_argument('--enable-quickack', help='Enable quick ACK',
                       action='store_true', dest='enable_quickack')
    group.add_argument('--disable-quickack', help='Disable quick ACK',
                       action='store_false', dest='enable_quickack')
    group = p.add_mutually_exclusive_group()
    group.add_argument('--enable-zerocopy-send-server', help='Enable zerocopy on send for server sockets',
                       action='store_true', dest='enable_zerocopy_send_server')
    group.add_argument('--disable-zerocopy-send-server', help='Disable zerocopy on send for server sockets',
                       action='store_false', dest='enable_zerocopy_send_server')
    group = p.add_mutually_exclusive_group()
    group.add_argument('--enable-zerocopy-send-client', help='Enable zerocopy on send for client sockets',
                       action='store_true', dest='enable_zerocopy_send_client')
    group.add_argument('--disable-zerocopy-send-client', help='Disable zerocopy on send for client sockets',
                       action='store_false', dest='enable_zerocopy_send_client')
    p.add_argument('--zerocopy-threshold', help='Set zerocopy_threshold in bytes', type=int)
    p.add_argument('--tls-version', help='TLS protocol version', type=int)
    group = p.add_mutually_exclusive_group()
    group.add_argument('--enable-ktls', help='Enable Kernel TLS',
                       action='store_true', dest='enable_ktls')
    group.add_argument('--disable-ktls', help='Disable Kernel TLS',
                       action='store_false', dest='enable_ktls')
    p.set_defaults(func=sock_impl_set_options, enable_recv_pipe=None, enable_quickack=None,
                   enable_placement_id=None, enable_zerocopy_send_server=None, enable_zerocopy_send_client=None,
                   zerocopy_threshold=None, tls_version=None, enable_ktls=None)

    def sock_set_default_impl(args):
        print_json(args.client.sock_set_default_impl(impl_name=args.impl))

    p = subparsers.add_parser('sock_set_default_impl', help="""Set the default sock implementation""")
    p.add_argument('-i', '--impl', help='Socket implementation name, e.g. posix', required=True)
    p.set_defaults(func=sock_set_default_impl)

    def sock_get_default_impl(args):
        print_json(args.client.sock_get_default_impl())

    p = subparsers.add_parser('sock_get_default_impl', help="Get the default sock implementation name")
    p.set_defaults(func=sock_get_default_impl)
