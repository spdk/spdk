#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2022 Intel Corporation.
#  All rights reserved.

from spdk.rpc.client import print_json


def perform_tests(args):
    print_json(args.client.call('perform_tests'))


def spdk_rpc_plugin_initialize(subparsers):
    p = subparsers.add_parser('perform_tests',
                              help='Returns true when hotplug apps starts running IO')
    p.set_defaults(func=perform_tests)
