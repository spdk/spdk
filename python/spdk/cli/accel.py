#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2016 Intel Corporation
#  All rights reserved.
#  Copyright (c) 2022 Dell Inc, or its subsidiaries.
#  Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
#

import sys
from spdk.rpc.client import print_dict, print_json, print_array  # noqa


def add_parser(subparsers):

    def accel_get_opc_assignments(args):
        print_dict(args.client.accel_get_opc_assignments())

    p = subparsers.add_parser('accel_get_opc_assignments', help='Get list of opcode name to module assignments.')
    p.set_defaults(func=accel_get_opc_assignments)

    def accel_get_module_info(args):
        print_dict(args.client.accel_get_module_info())

    p = subparsers.add_parser('accel_get_module_info',
                              help='Get list of valid module names and their operations.')
    p.set_defaults(func=accel_get_module_info)

    def accel_assign_opc(args):
        args.client.accel_assign_opc(opname=args.opname, module=args.module)

    p = subparsers.add_parser('accel_assign_opc', help='Manually assign an operation to a module.')
    p.add_argument('-o', '--opname', help='opname', required=True)
    p.add_argument('-m', '--module', help='name of module', required=True)
    p.set_defaults(func=accel_assign_opc)

    def accel_crypto_key_create(args):
        print_dict(args.client.accel_crypto_key_create(
                                                     cipher=args.cipher,
                                                     key=args.key,
                                                     key2=args.key2,
                                                     tweak_mode=args.tweak_mode,
                                                     name=args.name))

    p = subparsers.add_parser('accel_crypto_key_create', help='Create encryption key')
    p.add_argument('-c', '--cipher', help='cipher', required=True)
    p.add_argument('-k', '--key', help='key', required=True)
    p.add_argument('-e', '--key2', help='key2', default=None)
    p.add_argument('-t', '--tweak-mode', help='tweak mode', default=None)
    p.add_argument('-n', '--name', help='key name', required=True)
    p.set_defaults(func=accel_crypto_key_create)

    def accel_crypto_key_destroy(args):
        print_dict(args.client.accel_crypto_key_destroy(key_name=args.name))

    p = subparsers.add_parser('accel_crypto_key_destroy', help='Destroy encryption key')
    p.add_argument('-n', '--name', help='key name', required=True, type=str)
    p.set_defaults(func=accel_crypto_key_destroy)

    def accel_crypto_keys_get(args):
        print_dict(args.client.accel_crypto_keys_get(key_name=args.key_name))

    p = subparsers.add_parser('accel_crypto_keys_get', help='Get a list of the crypto keys')
    p.add_argument('-k', '--key-name', help='Get information about a specific key', type=str)
    p.set_defaults(func=accel_crypto_keys_get)

    def accel_set_driver(args):
        args.client.accel_set_driver(name=args.name)

    p = subparsers.add_parser('accel_set_driver', help='Select accel platform driver to execute ' +
                              'operation chains')
    p.add_argument('name', help='name of the platform driver')
    p.set_defaults(func=accel_set_driver)

    def accel_set_options(args):
        args.client.accel_set_options(small_cache_size=args.small_cache_size,
                                      large_cache_size=args.large_cache_size,
                                      task_count=args.task_count,
                                      sequence_count=args.sequence_count,
                                      buf_count=args.buf_count)

    p = subparsers.add_parser('accel_set_options', help='Set accel framework\'s options')
    p.add_argument('--small-cache-size', type=int, help='Size of the small iobuf cache')
    p.add_argument('--large-cache-size', type=int, help='Size of the large iobuf cache')
    p.add_argument('--task-count', type=int, help='Maximum number of tasks per IO channel')
    p.add_argument('--sequence-count', type=int, help='Maximum number of sequences per IO channel')
    p.add_argument('--buf-count', type=int, help='Maximum number of buffers per IO channel')
    p.set_defaults(func=accel_set_options)

    def accel_get_stats(args):
        print_dict(args.client.accel_get_stats())

    p = subparsers.add_parser('accel_get_stats', help='Display accel framework\'s statistics')
    p.set_defaults(func=accel_get_stats)
