#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) 2026 Nutanix Inc.

from spdk.rpc.cmd_parser import print_json


def check_changed_namespaces(args):
    print_json(args.client.call('check_changed_namespaces'))


def spdk_rpc_plugin_initialize(subparsers):
    p = subparsers.add_parser(
        'check_changed_namespaces',
        help=('Block until the AER test app reports the expected '
              'NS_ATTR_CHANGED event. Returns true on success and a JSON-RPC '
              'error otherwise.'))
    p.set_defaults(func=check_changed_namespaces)
