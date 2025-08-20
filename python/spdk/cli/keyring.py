#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2016 Intel Corporation
#  All rights reserved.
#  Copyright (c) 2022 Dell Inc, or its subsidiaries.
#  Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
#

import sys
from spdk.rpc.client import print_dict, print_json, print_array  # noqa


def add_parser(subparsers):

    def keyring_file_add_key(args):
        args.client.keyring_file_add_key(name=args.name, path=args.path)

    p = subparsers.add_parser('keyring_file_add_key', help='Add a file-based key to the keyring')
    p.add_argument('name', help='Name of the key to add')
    p.add_argument('path', help='Path of the file containing the key')
    p.set_defaults(func=keyring_file_add_key)

    def keyring_file_remove_key(args):
        args.client.keyring_file_remove_key(name=args.name)

    p = subparsers.add_parser('keyring_file_remove_key', help='Remove a file-based key from the keyring')
    p.add_argument('name', help='Name of the key to remove')
    p.set_defaults(func=keyring_file_remove_key)

    def keyring_get_keys(args):
        print_dict(args.client.keyring_get_keys())

    p = subparsers.add_parser('keyring_get_keys', help='Get a list of registered keys')
    p.set_defaults(func=keyring_get_keys)

    def keyring_linux_set_options(args):
        args.client.keyring_linux_set_options(enable=args.enable)

    p = subparsers.add_parser('keyring_linux_set_options', help='Set options of the keyring_linux module')
    p.add_argument('-e', '--enable', help='Enable keyring_linux module', action='store_true')
    p.set_defaults(func=keyring_linux_set_options)
