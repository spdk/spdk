#!/usr/bin/env python3
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2016 Intel Corporation
#  All rights reserved.
#  Copyright (c) 2022 Dell Inc, or its subsidiaries.
#  Copyright (c) 2022-2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
#

import logging
import argparse
import importlib
import os
import sys
import shlex
import json

try:
    from shlex import quote
except ImportError:
    from pipes import quote

sys.path.append(os.path.dirname(__file__) + '/../python')

import spdk.rpc as rpc  # noqa
from spdk.rpc.client import print_dict, print_json, JSONRPCException  # noqa
from spdk.rpc.helpers import deprecated_aliases  # noqa


def print_array(a):
    print(" ".join((quote(v) for v in a)))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description='SPDK RPC command line interface', usage='%(prog)s [options]')
    parser.add_argument('-s', dest='server_addr',
                        help='RPC domain socket path or IP address', default='/var/tmp/spdk.sock')
    parser.add_argument('-p', dest='port',
                        help='RPC port number (if server_addr is IP address)',
                        default=5260, type=int)
    parser.add_argument('-t', dest='timeout',
                        help='Timeout as a floating point number expressed in seconds waiting for response. Default: 60.0',
                        default=60.0, type=float)
    parser.add_argument('-r', dest='conn_retries',
                        help='Retry connecting to the RPC server N times with 0.2s interval. Default: 0',
                        default=0, type=int)
    parser.add_argument('-v', dest='verbose', action='store_const', const="INFO",
                        help='Set verbose mode to INFO', default="ERROR")
    parser.add_argument('--verbose', dest='verbose', choices=['DEBUG', 'INFO', 'ERROR'],
                        help="""Set verbose level. """)
    parser.add_argument('--dry-run', dest='dry_run', action='store_true', help="Display request and exit")
    parser.set_defaults(dry_run=False)
    parser.add_argument('--server', dest='is_server', action='store_true',
                        help="Start listening on stdin, parse each line as a regular rpc.py execution and create \
                                a separate connection for each command. Each command's output ends with either \
                                **STATUS=0 if the command succeeded or **STATUS=1 if it failed. --server is meant \
                                to be used in conjunction with bash coproc, where stdin and stdout are connected to \
                                pipes and can be used as a faster way to send RPC commands. If enabled, rpc.py \
                                must be executed without any other parameters.")
    parser.set_defaults(is_server=False)
    parser.add_argument('--plugin', dest='rpc_plugin', help='Module name of plugin with additional RPC commands')
    subparsers = parser.add_subparsers(help='RPC methods', dest='called_rpc_name', metavar='')

    def framework_start_init(args):
        rpc.framework_start_init(args.client)

    p = subparsers.add_parser('framework_start_init', help='Start initialization of subsystems')
    p.set_defaults(func=framework_start_init)

    def framework_wait_init(args):
        rpc.framework_wait_init(args.client)

    p = subparsers.add_parser('framework_wait_init', help='Block until subsystems have been initialized')
    p.set_defaults(func=framework_wait_init)

    def rpc_get_methods(args):
        print_dict(rpc.rpc_get_methods(args.client,
                                       current=args.current,
                                       include_aliases=args.include_aliases))

    p = subparsers.add_parser('rpc_get_methods', help='Get list of supported RPC methods')
    p.add_argument('-c', '--current', help='Get list of RPC methods only callable in the current state.', action='store_true')
    p.add_argument('-i', '--include-aliases', help='include RPC aliases', action='store_true')
    p.set_defaults(func=rpc_get_methods)

    def spdk_get_version(args):
        print_json(rpc.spdk_get_version(args.client))

    p = subparsers.add_parser('spdk_get_version', help='Get SPDK version')
    p.set_defaults(func=spdk_get_version)

    def save_config(args):
        rpc.save_config(args.client,
                        sys.stdout,
                        indent=args.indent)

    p = subparsers.add_parser('save_config', help="""Write current (live) configuration of SPDK subsystems and targets to stdout.
    """)
    p.add_argument('-i', '--indent', help="""Indent level. Value less than 0 mean compact mode. Default indent level is 2.
    """, type=int, default=2)
    p.set_defaults(func=save_config)

    def load_config(args):
        rpc.load_config(args.client, args.json_conf,
                        include_aliases=args.include_aliases)

    p = subparsers.add_parser('load_config', help="""Configure SPDK subsystems and targets using JSON RPC.""")
    p.add_argument('-i', '--include-aliases', help='include RPC aliases', action='store_true')
    p.add_argument('-j', '--json-conf', help='Valid JSON configuration', default=sys.stdin)
    p.set_defaults(func=load_config)

    def save_subsystem_config(args):
        rpc.save_subsystem_config(args.client,
                                  sys.stdout,
                                  indent=args.indent,
                                  name=args.name)

    p = subparsers.add_parser('save_subsystem_config', help="""Write current (live) configuration of SPDK subsystem to stdout.
    """)
    p.add_argument('-i', '--indent', help="""Indent level. Value less than 0 mean compact mode. Default indent level is 2.
    """, type=int, default=2)
    p.add_argument('-n', '--name', help='Name of subsystem', required=True)
    p.set_defaults(func=save_subsystem_config)

    def load_subsystem_config(args):
        rpc.load_subsystem_config(args.client,
                                  args.json_conf)

    p = subparsers.add_parser('load_subsystem_config', help="""Configure SPDK subsystem using JSON RPC.""")
    p.add_argument('-j', '--json-conf', help='Valid JSON configuration', default=sys.stdin)
    p.set_defaults(func=load_subsystem_config)

    # app
    def spdk_kill_instance(args):
        rpc.app.spdk_kill_instance(args.client,
                                   sig_name=args.sig_name)

    p = subparsers.add_parser('spdk_kill_instance', help='Send signal to instance')
    p.add_argument('sig_name', help='signal will be sent to server.')
    p.set_defaults(func=spdk_kill_instance)

    def framework_monitor_context_switch(args):
        enabled = None
        if args.enable:
            enabled = True
        if args.disable:
            enabled = False
        print_dict(rpc.app.framework_monitor_context_switch(args.client,
                                                            enabled=enabled))

    p = subparsers.add_parser('framework_monitor_context_switch',
                              help='Control whether the context switch monitor is enabled')
    p.add_argument('-e', '--enable', action='store_true', help='Enable context switch monitoring')
    p.add_argument('-d', '--disable', action='store_true', help='Disable context switch monitoring')
    p.set_defaults(func=framework_monitor_context_switch)

    def framework_get_reactors(args):
        print_dict(rpc.app.framework_get_reactors(args.client))

    p = subparsers.add_parser(
        'framework_get_reactors', help='Display list of all reactors')
    p.set_defaults(func=framework_get_reactors)

    def framework_set_scheduler(args):
        rpc.app.framework_set_scheduler(args.client,
                                        name=args.name,
                                        period=args.period,
                                        load_limit=args.load_limit,
                                        core_limit=args.core_limit,
                                        core_busy=args.core_busy)

    p = subparsers.add_parser(
        'framework_set_scheduler', help='Select thread scheduler that will be activated and its period (experimental)')
    p.add_argument('name', help="Name of a scheduler")
    p.add_argument('-p', '--period', help="Scheduler period in microseconds", type=int)
    p.add_argument('--load-limit', help="Scheduler load limit. Reserved for dynamic scheduler", type=int, required=False)
    p.add_argument('--core-limit', help="Scheduler core limit. Reserved for dynamic scheduler", type=int, required=False)
    p.add_argument('--core-busy', help="Scheduler core busy limit. Reserved for dynamic schedler", type=int, required=False)
    p.set_defaults(func=framework_set_scheduler)

    def framework_get_scheduler(args):
        print_dict(rpc.app.framework_get_scheduler(args.client))

    p = subparsers.add_parser(
        'framework_get_scheduler', help='Display currently set scheduler and its properties.')
    p.set_defaults(func=framework_get_scheduler)

    def framework_disable_cpumask_locks(args):
        rpc.framework_disable_cpumask_locks(args.client)

    p = subparsers.add_parser('framework_disable_cpumask_locks',
                              help='Disable CPU core lock files.')
    p.set_defaults(func=framework_disable_cpumask_locks)

    def framework_enable_cpumask_locks(args):
        rpc.framework_enable_cpumask_locks(args.client)

    p = subparsers.add_parser('framework_enable_cpumask_locks',
                              help='Enable CPU core lock files.')
    p.set_defaults(func=framework_enable_cpumask_locks)

    # bdev
    def bdev_set_options(args):
        rpc.bdev.bdev_set_options(args.client,
                                  bdev_io_pool_size=args.bdev_io_pool_size,
                                  bdev_io_cache_size=args.bdev_io_cache_size,
                                  bdev_auto_examine=args.bdev_auto_examine)

    p = subparsers.add_parser('bdev_set_options',
                              help="""Set options of bdev subsystem""")
    p.add_argument('-p', '--bdev-io-pool-size', help='Number of bdev_io structures in shared buffer pool', type=int)
    p.add_argument('-c', '--bdev-io-cache-size', help='Maximum number of bdev_io structures cached per thread', type=int)
    group = p.add_mutually_exclusive_group()
    group.add_argument('-e', '--enable-auto-examine', dest='bdev_auto_examine', help='Allow to auto examine', action='store_true')
    group.add_argument('-d', '--disable-auto-examine', dest='bdev_auto_examine', help='Not allow to auto examine', action='store_false')
    p.set_defaults(bdev_auto_examine=True)
    p.set_defaults(func=bdev_set_options)

    def bdev_examine(args):
        rpc.bdev.bdev_examine(args.client,
                              name=args.name)

    p = subparsers.add_parser('bdev_examine',
                              help="""examine a bdev if it exists, or will examine it after it is created""")
    p.add_argument('-b', '--name', help='Name or alias of the bdev')
    p.set_defaults(func=bdev_examine)

    def bdev_wait_for_examine(args):
        rpc.bdev.bdev_wait_for_examine(args.client)

    p = subparsers.add_parser('bdev_wait_for_examine',
                              help="""Report when all bdevs have been examined""")
    p.set_defaults(func=bdev_wait_for_examine)

    def bdev_compress_create(args):
        print_json(rpc.bdev.bdev_compress_create(args.client,
                                                 base_bdev_name=args.base_bdev_name,
                                                 pm_path=args.pm_path,
                                                 lb_size=args.lb_size))

    p = subparsers.add_parser('bdev_compress_create', help='Add a compress vbdev')
    p.add_argument('-b', '--base-bdev-name', help="Name of the base bdev")
    p.add_argument('-p', '--pm-path', help="Path to persistent memory")
    p.add_argument('-l', '--lb-size', help="Compressed vol logical block size (optional, if used must be 512 or 4096)", type=int)
    p.set_defaults(func=bdev_compress_create)

    def bdev_compress_delete(args):
        rpc.bdev.bdev_compress_delete(args.client,
                                      name=args.name)

    p = subparsers.add_parser('bdev_compress_delete', help='Delete a compress disk')
    p.add_argument('name', help='compress bdev name')
    p.set_defaults(func=bdev_compress_delete)

    def bdev_compress_get_orphans(args):
        print_dict(rpc.bdev.bdev_compress_get_orphans(args.client,
                                                      name=args.name))
    p = subparsers.add_parser(
        'bdev_compress_get_orphans', help='Display list of orphaned compress bdevs.')
    p.add_argument('-b', '--name', help="Name of a comp bdev. Example: COMP_Nvme0n1", required=False)
    p.set_defaults(func=bdev_compress_get_orphans)

    def bdev_crypto_create(args):
        print_json(rpc.bdev.bdev_crypto_create(args.client,
                                               base_bdev_name=args.base_bdev_name,
                                               name=args.name,
                                               crypto_pmd=args.crypto_pmd,
                                               key=args.key,
                                               cipher=args.cipher,
                                               key2=args.key2,
                                               key_name=args.key_name))
    p = subparsers.add_parser('bdev_crypto_create', help='Add a crypto vbdev')
    p.add_argument('base_bdev_name', help="Name of the base bdev")
    p.add_argument('name', help="Name of the crypto vbdev")
    p.add_argument('-p', '--crypto-pmd', help="Name of the crypto device driver. Obsolete, see dpdk_cryptodev_set_driver", required=False)
    p.add_argument('-k', '--key', help="Key. Obsolete, see accel_crypto_key_create", required=False)
    p.add_argument('-c', '--cipher', help="cipher to use. Obsolete, see accel_crypto_key_create", required=False)
    p.add_argument('-k2', '--key2', help="2nd key for cipher AES_XTS. Obsolete, see accel_crypto_key_create", default=None)
    p.add_argument('-n', '--key-name', help="Key name to use, see accel_crypto_key_create", required=False)
    p.set_defaults(func=bdev_crypto_create)

    def bdev_crypto_delete(args):
        rpc.bdev.bdev_crypto_delete(args.client,
                                    name=args.name)

    p = subparsers.add_parser('bdev_crypto_delete', help='Delete a crypto disk')
    p.add_argument('name', help='crypto bdev name')
    p.set_defaults(func=bdev_crypto_delete)

    def bdev_ocf_create(args):
        print_json(rpc.bdev.bdev_ocf_create(args.client,
                                            name=args.name,
                                            mode=args.mode,
                                            cache_line_size=args.cache_line_size,
                                            cache_bdev_name=args.cache_bdev_name,
                                            core_bdev_name=args.core_bdev_name))
    p = subparsers.add_parser('bdev_ocf_create', help='Add an OCF block device')
    p.add_argument('name', help='Name of resulting OCF bdev')
    p.add_argument('mode', help='OCF cache mode', choices=['wb', 'wt', 'pt', 'wa', 'wi', 'wo'])
    p.add_argument(
        '--cache-line-size',
        help='OCF cache line size. The unit is KiB',
        type=int,
        choices=[4, 8, 16, 32, 64],
        required=False
    )
    p.add_argument('cache_bdev_name', help='Name of underlying cache bdev')
    p.add_argument('core_bdev_name', help='Name of underlying core bdev')
    p.set_defaults(func=bdev_ocf_create)

    def bdev_ocf_delete(args):
        rpc.bdev.bdev_ocf_delete(args.client,
                                 name=args.name)

    p = subparsers.add_parser('bdev_ocf_delete', help='Delete an OCF block device')
    p.add_argument('name', help='Name of OCF bdev')
    p.set_defaults(func=bdev_ocf_delete)

    def bdev_ocf_get_stats(args):
        print_dict(rpc.bdev.bdev_ocf_get_stats(args.client,
                                               name=args.name))
    p = subparsers.add_parser('bdev_ocf_get_stats', help='Get statistics of chosen OCF block device')
    p.add_argument('name', help='Name of OCF bdev')
    p.set_defaults(func=bdev_ocf_get_stats)

    def bdev_ocf_reset_stats(args):
        print_dict(rpc.bdev.bdev_ocf_reset_stats(args.client,
                                                 name=args.name))
    p = subparsers.add_parser('bdev_ocf_reset_stats', help='Reset statistics of chosen OCF block device')
    p.add_argument('name', help='Name of OCF bdev')
    p.set_defaults(func=bdev_ocf_reset_stats)

    def bdev_ocf_get_bdevs(args):
        print_dict(rpc.bdev.bdev_ocf_get_bdevs(args.client,
                                               name=args.name))
    p = subparsers.add_parser('bdev_ocf_get_bdevs', help='Get list of OCF devices including unregistered ones')
    p.add_argument('name', nargs='?', help='name of OCF vbdev or name of cache device or name of core device (optional)')
    p.set_defaults(func=bdev_ocf_get_bdevs)

    def bdev_ocf_set_cache_mode(args):
        print_json(rpc.bdev.bdev_ocf_set_cache_mode(args.client,
                                                    name=args.name,
                                                    mode=args.mode))
    p = subparsers.add_parser('bdev_ocf_set_cache_mode',
                              help='Set cache mode of OCF block device')
    p.add_argument('name', help='Name of OCF bdev')
    p.add_argument('mode', help='OCF cache mode', choices=['wb', 'wt', 'pt', 'wa', 'wi', 'wo'])
    p.set_defaults(func=bdev_ocf_set_cache_mode)

    def bdev_ocf_set_seqcutoff(args):
        rpc.bdev.bdev_ocf_set_seqcutoff(args.client,
                                        name=args.name,
                                        policy=args.policy,
                                        threshold=args.threshold,
                                        promotion_count=args.promotion_count)
    p = subparsers.add_parser('bdev_ocf_set_seqcutoff',
                              help='Set sequential cutoff parameters on all cores for the given OCF cache device')
    p.add_argument('name', help='Name of OCF cache bdev')
    p.add_argument('-t', '--threshold', type=int,
                   help='Activation threshold [KiB]')
    p.add_argument('-c', '--promotion-count', type=int,
                   help='Promotion request count')
    p.add_argument('-p', '--policy', choices=['always', 'full', 'never'], required=True,
                   help='Sequential cutoff policy')
    p.set_defaults(func=bdev_ocf_set_seqcutoff)

    def bdev_ocf_flush_start(args):
        rpc.bdev.bdev_ocf_flush_start(args.client, name=args.name)
    p = subparsers.add_parser('bdev_ocf_flush_start',
                              help='Start flushing OCF cache device')
    p.add_argument('name', help='Name of OCF bdev')
    p.set_defaults(func=bdev_ocf_flush_start)

    def bdev_ocf_flush_status(args):
        print_json(rpc.bdev.bdev_ocf_flush_status(args.client, name=args.name))
    p = subparsers.add_parser('bdev_ocf_flush_status',
                              help='Get flush status of OCF cache device')
    p.add_argument('name', help='Name of OCF bdev')
    p.set_defaults(func=bdev_ocf_flush_status)

    def bdev_malloc_create(args):
        num_blocks = (args.total_size * 1024 * 1024) // args.block_size
        print_json(rpc.bdev.bdev_malloc_create(args.client,
                                               num_blocks=int(num_blocks),
                                               block_size=args.block_size,
                                               physical_block_size=args.physical_block_size,
                                               name=args.name,
                                               uuid=args.uuid,
                                               optimal_io_boundary=args.optimal_io_boundary,
                                               md_size=args.md_size,
                                               md_interleave=args.md_interleave,
                                               dif_type=args.dif_type,
                                               dif_is_head_of_md=args.dif_is_head_of_md))
    p = subparsers.add_parser('bdev_malloc_create', help='Create a bdev with malloc backend')
    p.add_argument('-b', '--name', help="Name of the bdev")
    p.add_argument('-u', '--uuid', help="UUID of the bdev")
    p.add_argument(
        'total_size', help='Size of malloc bdev in MB (float > 0)', type=float)
    p.add_argument('block_size', help='Data block size for this bdev', type=int)
    p.add_argument('-p', '--physical-block-size', help='Physical block size for this bdev.', type=int)
    p.add_argument('-o', '--optimal-io-boundary', help="""Split on optimal IO boundary, in number of
    blocks, default 0 (disabled)""", type=int)
    p.add_argument('-m', '--md-size', type=int,
                   help='Metadata size for this bdev (0, 8, 16, 32, 64, or 128). Default is 0.')
    p.add_argument('-i', '--md-interleave', action='store_true',
                   help='Metadata location, interleaved if set, and separated if omitted.')
    p.add_argument('-t', '--dif-type', type=int, choices=[0, 1, 2, 3],
                   help='Protection information type. Parameter --md-size needs'
                        'to be set along --dif-type. Default=0 - no protection.')
    p.add_argument('-d', '--dif-is-head-of-md', action='store_true',
                   help='Protection information is in the first 8 bytes of metadata. Default=false.')
    p.set_defaults(func=bdev_malloc_create)

    def bdev_malloc_delete(args):
        rpc.bdev.bdev_malloc_delete(args.client,
                                    name=args.name)

    p = subparsers.add_parser('bdev_malloc_delete', help='Delete a malloc disk')
    p.add_argument('name', help='malloc bdev name')
    p.set_defaults(func=bdev_malloc_delete)

    def bdev_null_create(args):
        num_blocks = (args.total_size * 1024 * 1024) // args.block_size
        if args.dif_type and not args.md_size:
            print("ERROR: --md-size must be > 0 when --dif-type is > 0")
            exit(1)
        print_json(rpc.bdev.bdev_null_create(args.client,
                                             num_blocks=num_blocks,
                                             block_size=args.block_size,
                                             physical_block_size=args.physical_block_size,
                                             name=args.name,
                                             uuid=args.uuid,
                                             md_size=args.md_size,
                                             dif_type=args.dif_type,
                                             dif_is_head_of_md=args.dif_is_head_of_md))

    p = subparsers.add_parser('bdev_null_create', help='Add a bdev with null backend')
    p.add_argument('name', help='Block device name')
    p.add_argument('-u', '--uuid', help='UUID of the bdev')
    p.add_argument('total_size', help='Size of null bdev in MB (int > 0). Includes only data blocks.', type=int)
    p.add_argument('block_size', help='Block size for this bdev.'
                                      'Should be a sum of block size and metadata size if --md-size is used.', type=int)
    p.add_argument('-p', '--physical-block-size', help='Physical block size for this bdev.', type=int)
    p.add_argument('-m', '--md-size', type=int,
                   help='Metadata size for this bdev. Default=0.')
    p.add_argument('-t', '--dif-type', type=int, choices=[0, 1, 2, 3],
                   help='Protection information type. Parameter --md-size needs'
                        'to be set along --dif-type. Default=0 - no protection.')
    p.add_argument('-d', '--dif-is-head-of-md', action='store_true',
                   help='Protection information is in the first 8 bytes of metadata. Default=false.')
    p.set_defaults(func=bdev_null_create)

    def bdev_null_delete(args):
        rpc.bdev.bdev_null_delete(args.client,
                                  name=args.name)

    p = subparsers.add_parser('bdev_null_delete', help='Delete a null bdev')
    p.add_argument('name', help='null bdev name')
    p.set_defaults(func=bdev_null_delete)

    def bdev_null_resize(args):
        print_json(rpc.bdev.bdev_null_resize(args.client,
                                             name=args.name,
                                             new_size=int(args.new_size)))

    p = subparsers.add_parser('bdev_null_resize',
                              help='Resize a null bdev')
    p.add_argument('name', help='null bdev name')
    p.add_argument('new_size', help='new bdev size for resize operation. The unit is MiB')
    p.set_defaults(func=bdev_null_resize)

    def bdev_aio_create(args):
        print_json(rpc.bdev.bdev_aio_create(args.client,
                                            filename=args.filename,
                                            name=args.name,
                                            block_size=args.block_size,
                                            readonly=args.readonly))

    p = subparsers.add_parser('bdev_aio_create', help='Add a bdev with aio backend')
    p.add_argument('filename', help='Path to device or file (ex: /dev/sda)')
    p.add_argument('name', help='Block device name')
    p.add_argument('block_size', help='Block size for this bdev', type=int, nargs='?')
    p.add_argument("-r", "--readonly", action='store_true', help='Set this bdev as read-only')
    p.set_defaults(func=bdev_aio_create)

    def bdev_aio_rescan(args):
        print_json(rpc.bdev.bdev_aio_rescan(args.client,
                                            name=args.name))

    p = subparsers.add_parser('bdev_aio_rescan', help='Rescan a bdev size with aio backend')
    p.add_argument('name', help='Block device name')
    p.set_defaults(func=bdev_aio_rescan)

    def bdev_aio_delete(args):
        rpc.bdev.bdev_aio_delete(args.client,
                                 name=args.name)

    p = subparsers.add_parser('bdev_aio_delete', help='Delete an aio disk')
    p.add_argument('name', help='aio bdev name')
    p.set_defaults(func=bdev_aio_delete)

    def bdev_uring_create(args):
        print_json(rpc.bdev.bdev_uring_create(args.client,
                                              filename=args.filename,
                                              name=args.name,
                                              block_size=args.block_size))

    p = subparsers.add_parser('bdev_uring_create', help='Create a bdev with io_uring backend')
    p.add_argument('filename', help='Path to device or file (ex: /dev/nvme0n1)')
    p.add_argument('name', help='bdev name')
    p.add_argument('block_size', help='Block size for this bdev', type=int, nargs='?')
    p.set_defaults(func=bdev_uring_create)

    def bdev_uring_delete(args):
        rpc.bdev.bdev_uring_delete(args.client,
                                   name=args.name)

    p = subparsers.add_parser('bdev_uring_delete', help='Delete a uring bdev')
    p.add_argument('name', help='uring bdev name')
    p.set_defaults(func=bdev_uring_delete)

    def bdev_xnvme_create(args):
        print_json(rpc.bdev.bdev_xnvme_create(args.client,
                                              filename=args.filename,
                                              name=args.name,
                                              io_mechanism=args.io_mechanism))

    p = subparsers.add_parser('bdev_xnvme_create', help='Create a bdev with xNVMe backend')
    p.add_argument('filename', help='Path to device or file (ex: /dev/nvme0n1)')
    p.add_argument('name', help='name of xNVMe bdev to create')
    p.add_argument('io_mechanism', help='IO mechanism to use (ex: libaio, io_uring, io_uring_cmd, etc.)')
    p.add_argument('conserve_cpu', action='store_true', help='Whether or not to conserve CPU when polling')
    p.set_defaults(func=bdev_xnvme_create)

    def bdev_xnvme_delete(args):
        rpc.bdev.bdev_xnvme_delete(args.client,
                                   name=args.name)

    p = subparsers.add_parser('bdev_xnvme_delete', help='Delete a xNVMe bdev')
    p.add_argument('name', help='xNVMe bdev name')
    p.set_defaults(func=bdev_xnvme_delete)

    def bdev_nvme_set_options(args):
        rpc.bdev.bdev_nvme_set_options(args.client,
                                       action_on_timeout=args.action_on_timeout,
                                       timeout_us=args.timeout_us,
                                       timeout_admin_us=args.timeout_admin_us,
                                       keep_alive_timeout_ms=args.keep_alive_timeout_ms,
                                       retry_count=args.retry_count,
                                       arbitration_burst=args.arbitration_burst,
                                       low_priority_weight=args.low_priority_weight,
                                       medium_priority_weight=args.medium_priority_weight,
                                       high_priority_weight=args.high_priority_weight,
                                       nvme_adminq_poll_period_us=args.nvme_adminq_poll_period_us,
                                       nvme_ioq_poll_period_us=args.nvme_ioq_poll_period_us,
                                       io_queue_requests=args.io_queue_requests,
                                       delay_cmd_submit=args.delay_cmd_submit,
                                       transport_retry_count=args.transport_retry_count,
                                       bdev_retry_count=args.bdev_retry_count,
                                       transport_ack_timeout=args.transport_ack_timeout,
                                       ctrlr_loss_timeout_sec=args.ctrlr_loss_timeout_sec,
                                       reconnect_delay_sec=args.reconnect_delay_sec,
                                       fast_io_fail_timeout_sec=args.fast_io_fail_timeout_sec,
                                       disable_auto_failback=args.disable_auto_failback,
                                       generate_uuids=args.generate_uuids,
                                       transport_tos=args.transport_tos,
                                       nvme_error_stat=args.nvme_error_stat,
                                       rdma_srq_size=args.rdma_srq_size,
                                       io_path_stat=args.io_path_stat)

    p = subparsers.add_parser('bdev_nvme_set_options',
                              help='Set options for the bdev nvme type. This is startup command.')
    p.add_argument('-a', '--action-on-timeout',
                   help="Action to take on command time out. Valid values are: none, reset, abort")
    p.add_argument('-t', '--timeout-us',
                   help="Timeout for each command, in microseconds. If 0, don't track timeouts.", type=int)
    p.add_argument('--timeout-admin-us',
                   help="Timeout for each admin command, in microseconds. If 0, treat same as io timeouts.", type=int)
    p.add_argument('-k', '--keep-alive-timeout-ms',
                   help="Keep alive timeout period in millisecond. If 0, disable keep-alive.", type=int)
    p.add_argument('-n', '--retry-count',
                   help='the number of attempts per I/O when an I/O fails. (deprecated, please use --transport-retry-count.)', type=int)
    p.add_argument('--arbitration-burst',
                   help='the value is expressed as a power of two', type=int)
    p.add_argument('--low-priority-weight',
                   help='the maximum number of commands that the controller may launch at one time from a low priority queue', type=int)
    p.add_argument('--medium-priority-weight',
                   help='the maximum number of commands that the controller may launch at one time from a medium priority queue', type=int)
    p.add_argument('--high-priority-weight',
                   help='the maximum number of commands that the controller may launch at one time from a high priority queue', type=int)
    p.add_argument('-p', '--nvme-adminq-poll-period-us',
                   help='How often the admin queue is polled for asynchronous events', type=int)
    p.add_argument('-i', '--nvme-ioq-poll-period-us',
                   help='How often to poll I/O queues for completions', type=int)
    p.add_argument('-s', '--io-queue-requests',
                   help='The number of requests allocated for each NVMe I/O queue. Default: 512', type=int)
    p.add_argument('-d', '--disable-delay-cmd-submit',
                   help='Disable delaying NVMe command submission, i.e. no batching of multiple commands',
                   action='store_false', dest='delay_cmd_submit')
    p.add_argument('-c', '--transport-retry-count',
                   help='the number of attempts per I/O in the transport layer when an I/O fails.', type=int)
    p.add_argument('-r', '--bdev-retry-count',
                   help='the number of attempts per I/O in the bdev layer when an I/O fails. -1 means infinite retries.', type=int)
    p.add_argument('-e', '--transport-ack-timeout',
                   help="""Time to wait ack until packet retransmission for RDMA or until closes connection for TCP.
                   Range 0-31 where 0 is driver-specific default value.""", type=int)
    p.add_argument('-l', '--ctrlr-loss-timeout-sec',
                   help="""Time to wait until ctrlr is reconnected before deleting ctrlr.
                   -1 means infinite reconnect retries. 0 means no reconnect retry.
                   If reconnect_delay_sec is zero, ctrlr_loss_timeout_sec has to be zero.
                   If reconnect_delay_sec is non-zero, ctrlr_loss_timeout_sec has to be -1 or not less than
                   reconnect_delay_sec.
                   This can be overridden by bdev_nvme_attach_controller.""",
                   type=int)
    p.add_argument('-o', '--reconnect-delay-sec',
                   help="""Time to delay a reconnect retry.
                   If ctrlr_loss_timeout_sec is zero, reconnect_delay_sec has to be zero.
                   If ctrlr_loss_timeout_sec is -1, reconnect_delay_sec has to be non-zero.
                   If ctrlr_loss_timeout_sec is not -1 or zero, reconnect_delay_sec has to be non-zero and
                   less than ctrlr_loss_timeout_sec.
                   This can be overridden by bdev_nvme_attach_controller.""",
                   type=int)
    p.add_argument('-u', '--fast-io-fail-timeout-sec',
                   help="""Time to wait until ctrlr is reconnected before failing I/O to ctrlr.
                   0 means no such timeout.
                   If fast_io_fail_timeout_sec is not zero, it has to be not less than reconnect_delay_sec and
                   less than ctrlr_loss_timeout_sec if ctrlr_loss_timeout_sec is not -1.
                   This can be overridden by bdev_nvme_attach_controller.""",
                   type=int)
    p.add_argument('-f', '--disable-auto-failback',
                   help="""Disable automatic failback. bdev_nvme_set_preferred_path can be used to do manual failback.
                   By default, immediately failback to the preferred I/O path if it restored.""",
                   action='store_true')
    p.add_argument('--generate-uuids',
                   help="""Enable generation of unique identifiers for NVMe bdevs only if they do
                   not provide UUID themselves. These strings are based on device serial number and
                   namespace ID and will always be the same for that device.""", action='store_true')
    p.add_argument('--transport-tos',
                   help="""IPv4 Type of Service value. Only applicable for RDMA transports.
                   The default is 0 which means no TOS is applied.""", type=int)
    p.add_argument('-m', '--nvme-error-stat', help="Enable collecting NVMe error counts.", action='store_true')
    p.add_argument('-q', '--rdma-srq-size',
                   help='Set the size of a shared rdma receive queue. Default: 0 (disabled)', type=int)
    p.add_argument('--io-path-stat',
                   help="""Enable collecting I/O path stat of each io path.""",
                   action='store_true')

    p.set_defaults(func=bdev_nvme_set_options)

    def bdev_nvme_set_hotplug(args):
        rpc.bdev.bdev_nvme_set_hotplug(args.client, enable=args.enable, period_us=args.period_us)

    p = subparsers.add_parser('bdev_nvme_set_hotplug', help='Set hotplug options for bdev nvme type.')
    p.add_argument('-d', '--disable', dest='enable', default=False, action='store_false', help="Disable hotplug (default)")
    p.add_argument('-e', '--enable', dest='enable', action='store_true', help="Enable hotplug")
    p.add_argument('-r', '--period-us',
                   help='How often the hotplug is processed for insert and remove events', type=int)
    p.set_defaults(func=bdev_nvme_set_hotplug)

    def bdev_nvme_attach_controller(args):
        print_array(rpc.bdev.bdev_nvme_attach_controller(args.client,
                                                         name=args.name,
                                                         trtype=args.trtype,
                                                         traddr=args.traddr,
                                                         adrfam=args.adrfam,
                                                         trsvcid=args.trsvcid,
                                                         priority=args.priority,
                                                         subnqn=args.subnqn,
                                                         hostnqn=args.hostnqn,
                                                         hostaddr=args.hostaddr,
                                                         hostsvcid=args.hostsvcid,
                                                         prchk_reftag=args.prchk_reftag,
                                                         prchk_guard=args.prchk_guard,
                                                         hdgst=args.hdgst,
                                                         ddgst=args.ddgst,
                                                         fabrics_timeout=args.fabrics_timeout,
                                                         multipath=args.multipath,
                                                         num_io_queues=args.num_io_queues,
                                                         ctrlr_loss_timeout_sec=args.ctrlr_loss_timeout_sec,
                                                         reconnect_delay_sec=args.reconnect_delay_sec,
                                                         fast_io_fail_timeout_sec=args.fast_io_fail_timeout_sec,
                                                         psk=args.psk,
                                                         max_bdevs=args.max_bdevs))

    p = subparsers.add_parser('bdev_nvme_attach_controller', help='Add bdevs with nvme backend')
    p.add_argument('-b', '--name', help="Name of the NVMe controller, prefix for each bdev name", required=True)
    p.add_argument('-t', '--trtype',
                   help='NVMe-oF target trtype: e.g., rdma, pcie', required=True)
    p.add_argument('-a', '--traddr',
                   help='NVMe-oF target address: e.g., an ip address or BDF', required=True)
    p.add_argument('-f', '--adrfam',
                   help='NVMe-oF target adrfam: e.g., ipv4, ipv6, ib, fc, intra_host')
    p.add_argument('-s', '--trsvcid',
                   help='NVMe-oF target trsvcid: e.g., a port number')
    p.add_argument('-p', '--priority',
                   help='NVMe-oF connection priority: e.g., a priority number')
    p.add_argument('-n', '--subnqn', help='NVMe-oF target subnqn')
    p.add_argument('-q', '--hostnqn', help='NVMe-oF host subnqn')
    p.add_argument('-i', '--hostaddr',
                   help='NVMe-oF host address: e.g., an ip address')
    p.add_argument('-c', '--hostsvcid',
                   help='NVMe-oF host svcid: e.g., a port number')
    p.add_argument('-r', '--prchk-reftag',
                   help='Enable checking of PI reference tag for I/O processing.', action='store_true')
    p.add_argument('-g', '--prchk-guard',
                   help='Enable checking of PI guard for I/O processing.', action='store_true')
    p.add_argument('-e', '--hdgst',
                   help='Enable TCP header digest.', action='store_true')
    p.add_argument('-d', '--ddgst',
                   help='Enable TCP data digest.', action='store_true')
    p.add_argument('--fabrics-timeout', type=int, help='Fabrics connect timeout in microseconds')
    p.add_argument('-x', '--multipath', help='Set multipath behavior (disable, failover, multipath)')
    p.add_argument('--num-io-queues', type=int, help='Set the number of IO queues to request during initialization.')
    p.add_argument('-l', '--ctrlr-loss-timeout-sec',
                   help="""Time to wait until ctrlr is reconnected before deleting ctrlr.
                   -1 means infinite reconnect retries. 0 means no reconnect retry.
                   If reconnect_delay_sec is zero, ctrlr_loss_timeout_sec has to be zero.
                   If reconnect_delay_sec is non-zero, ctrlr_loss_timeout_sec has to be -1 or not less than
                   reconnect_delay_sec.""",
                   type=int)
    p.add_argument('-o', '--reconnect-delay-sec',
                   help="""Time to delay a reconnect retry.
                   If ctrlr_loss_timeout_sec is zero, reconnect_delay_sec has to be zero.
                   If ctrlr_loss_timeout_sec is -1, reconnect_delay_sec has to be non-zero.
                   If ctrlr_loss_timeout_sec is not -1 or zero, reconnect_delay_sec has to be non-zero and
                   less than ctrlr_loss_timeout_sec.""",
                   type=int)
    p.add_argument('-u', '--fast-io-fail-timeout-sec',
                   help="""Time to wait until ctrlr is reconnected before failing I/O to ctrlr.
                   0 means no such timeout.
                   If fast_io_fail_timeout_sec is not zero, it has to be not less than reconnect_delay_sec and
                   less than ctrlr_loss_timeout_sec if ctrlr_loss_timeout_sec is not -1.""",
                   type=int)
    p.add_argument('-k', '--psk',
                   help='Set PSK file path and enable TCP SSL socket implementation.')
    p.add_argument('-m', '--max-bdevs', type=int,
                   help='The size of the name array for newly created bdevs. Default is 128',)

    p.set_defaults(func=bdev_nvme_attach_controller)

    def bdev_nvme_get_controllers(args):
        print_dict(rpc.nvme.bdev_nvme_get_controllers(args.client,
                                                      name=args.name))

    p = subparsers.add_parser(
        'bdev_nvme_get_controllers', help='Display current NVMe controllers list or required NVMe controller')
    p.add_argument('-n', '--name', help="Name of the NVMe controller. Example: Nvme0", required=False)
    p.set_defaults(func=bdev_nvme_get_controllers)

    def bdev_nvme_detach_controller(args):
        rpc.bdev.bdev_nvme_detach_controller(args.client,
                                             name=args.name,
                                             trtype=args.trtype,
                                             traddr=args.traddr,
                                             adrfam=args.adrfam,
                                             trsvcid=args.trsvcid,
                                             subnqn=args.subnqn,
                                             hostaddr=args.hostaddr,
                                             hostsvcid=args.hostsvcid)

    p = subparsers.add_parser('bdev_nvme_detach_controller',
                              help='Detach an NVMe controller and delete any associated bdevs')
    p.add_argument('name', help="Name of the controller")
    p.add_argument('-t', '--trtype',
                   help='NVMe-oF target trtype: e.g., rdma, pcie')
    p.add_argument('-a', '--traddr',
                   help='NVMe-oF target address: e.g., an ip address or BDF')
    p.add_argument('-f', '--adrfam',
                   help='NVMe-oF target adrfam: e.g., ipv4, ipv6, ib, fc, intra_host')
    p.add_argument('-s', '--trsvcid',
                   help='NVMe-oF target trsvcid: e.g., a port number')
    p.add_argument('-n', '--subnqn', help='NVMe-oF target subnqn')
    p.add_argument('-i', '--hostaddr',
                   help='NVMe-oF host address: e.g., an ip address')
    p.add_argument('-c', '--hostsvcid',
                   help='NVMe-oF host svcid: e.g., a port number')
    p.set_defaults(func=bdev_nvme_detach_controller)

    def bdev_nvme_reset_controller(args):
        rpc.bdev.bdev_nvme_reset_controller(args.client, name=args.name)

    p = subparsers.add_parser('bdev_nvme_reset_controller',
                              help='Reset an NVMe controller')
    p.add_argument('name', help="Name of the NVMe controller")
    p.set_defaults(func=bdev_nvme_reset_controller)

    def bdev_nvme_start_discovery(args):
        rpc.bdev.bdev_nvme_start_discovery(args.client,
                                           name=args.name,
                                           trtype=args.trtype,
                                           traddr=args.traddr,
                                           adrfam=args.adrfam,
                                           trsvcid=args.trsvcid,
                                           hostnqn=args.hostnqn,
                                           wait_for_attach=args.wait_for_attach,
                                           attach_timeout_ms=args.attach_timeout_ms,
                                           ctrlr_loss_timeout_sec=args.ctrlr_loss_timeout_sec,
                                           reconnect_delay_sec=args.reconnect_delay_sec,
                                           fast_io_fail_timeout_sec=args.fast_io_fail_timeout_sec)

    p = subparsers.add_parser('bdev_nvme_start_discovery', help='Start automatic discovery')
    p.add_argument('-b', '--name', help="Name of the NVMe controller prefix for each bdev name", required=True)
    p.add_argument('-t', '--trtype',
                   help='NVMe-oF target trtype: e.g., rdma, pcie', required=True)
    p.add_argument('-a', '--traddr',
                   help='NVMe-oF target address: e.g., an ip address or BDF', required=True)
    p.add_argument('-f', '--adrfam',
                   help='NVMe-oF target adrfam: e.g., ipv4, ipv6, ib, fc, intra_host')
    p.add_argument('-s', '--trsvcid',
                   help='NVMe-oF target trsvcid: e.g., a port number')
    p.add_argument('-q', '--hostnqn', help='NVMe-oF host subnqn')
    p.add_argument('-w', '--wait-for-attach', action='store_true',
                   help='Do not complete RPC until all discovered NVM subsystems are attached')
    p.add_argument('-T', '--attach-timeout-ms', type=int, required=False,
                   help="""Time to wait until the discovery and all discovered NVM subsystems
                        are attached (default: 0, meaning wait indefinitely).  Automatically
                        selects the --wait-for-attach option.""")
    p.add_argument('-l', '--ctrlr-loss-timeout-sec',
                   help="""Time to wait until ctrlr is reconnected before deleting ctrlr.
                   -1 means infinite reconnect retries. 0 means no reconnect retry.
                   If reconnect_delay_sec is zero, ctrlr_loss_timeout_sec has to be zero.
                   If reconnect_delay_sec is non-zero, ctrlr_loss_timeout_sec has to be -1 or not less than
                   reconnect_delay_sec.""",
                   type=int)
    p.add_argument('-o', '--reconnect-delay-sec',
                   help="""Time to delay a reconnect retry.
                   If ctrlr_loss_timeout_sec is zero, reconnect_delay_sec has to be zero.
                   If ctrlr_loss_timeout_sec is -1, reconnect_delay_sec has to be non-zero.
                   If ctrlr_loss_timeout_sec is not -1 or zero, reconnect_delay_sec has to be non-zero and
                   less than ctrlr_loss_timeout_sec.""",
                   type=int)
    p.add_argument('-u', '--fast-io-fail-timeout-sec',
                   help="""Time to wait until ctrlr is reconnected before failing I/O to ctrlr.
                   0 means no such timeout.
                   If fast_io_fail_timeout_sec is not zero, it has to be not less than reconnect_delay_sec and
                   less than ctrlr_loss_timeout_sec if ctrlr_loss_timeout_sec is not -1.""",
                   type=int)
    p.set_defaults(func=bdev_nvme_start_discovery)

    def bdev_nvme_stop_discovery(args):
        rpc.bdev.bdev_nvme_stop_discovery(args.client, name=args.name)

    p = subparsers.add_parser('bdev_nvme_stop_discovery', help='Stop automatic discovery')
    p.add_argument('-b', '--name', help="Name of the service to stop", required=True)
    p.set_defaults(func=bdev_nvme_stop_discovery)

    def bdev_nvme_get_discovery_info(args):
        print_dict(rpc.bdev.bdev_nvme_get_discovery_info(args.client))

    p = subparsers.add_parser('bdev_nvme_get_discovery_info', help='Get information about the automatic discovery')
    p.set_defaults(func=bdev_nvme_get_discovery_info)

    def bdev_nvme_get_io_paths(args):
        print_dict(rpc.bdev.bdev_nvme_get_io_paths(args.client, name=args.name))

    p = subparsers.add_parser('bdev_nvme_get_io_paths', help='Display active I/O paths')
    p.add_argument('-n', '--name', help="Name of the NVMe bdev", required=False)
    p.set_defaults(func=bdev_nvme_get_io_paths)

    def bdev_nvme_set_preferred_path(args):
        rpc.bdev.bdev_nvme_set_preferred_path(args.client,
                                              name=args.name,
                                              cntlid=args.cntlid)

    p = subparsers.add_parser('bdev_nvme_set_preferred_path',
                              help="""Set the preferred I/O path for an NVMe bdev when in multipath mode""")
    p.add_argument('-b', '--name', help='Name of the NVMe bdev', required=True)
    p.add_argument('-c', '--cntlid', help='NVMe-oF controller ID', type=int, required=True)
    p.set_defaults(func=bdev_nvme_set_preferred_path)

    def bdev_nvme_set_multipath_policy(args):
        rpc.bdev.bdev_nvme_set_multipath_policy(args.client,
                                                name=args.name,
                                                policy=args.policy,
                                                selector=args.selector,
                                                rr_min_io=args.rr_min_io)

    p = subparsers.add_parser('bdev_nvme_set_multipath_policy',
                              help="""Set multipath policy of the NVMe bdev""")
    p.add_argument('-b', '--name', help='Name of the NVMe bdev', required=True)
    p.add_argument('-p', '--policy', help='Multipath policy (active_passive or active_active)', required=True)
    p.add_argument('-s', '--selector', help='Multipath selector (round_robin, queue_depth)', required=False)
    p.add_argument('-r', '--rr-min-io',
                   help='Number of IO to route to a path before switching to another for round-robin',
                   type=int, required=False)
    p.set_defaults(func=bdev_nvme_set_multipath_policy)

    def bdev_nvme_get_path_iostat(args):
        print_dict(rpc.bdev.bdev_nvme_get_path_iostat(args.client,
                                                      name=args.name))

    p = subparsers.add_parser('bdev_nvme_get_path_iostat',
                              help="""Display current I/O statistics of all the IO paths of the blockdev. It can be
                              called when io_path_stat is true.""")
    p.add_argument('-b', '--name', help="Name of the Blockdev. Example: NVMe0n1", required=True)
    p.set_defaults(func=bdev_nvme_get_path_iostat)

    def bdev_nvme_cuse_register(args):
        rpc.bdev.bdev_nvme_cuse_register(args.client,
                                         name=args.name)

    p = subparsers.add_parser('bdev_nvme_cuse_register',
                              help='Register CUSE devices on NVMe controller')
    p.add_argument('-n', '--name',
                   help='Name of the NVMe controller. Example: Nvme0', required=True)
    p.set_defaults(func=bdev_nvme_cuse_register)

    def bdev_nvme_cuse_unregister(args):
        rpc.bdev.bdev_nvme_cuse_unregister(args.client,
                                           name=args.name)

    p = subparsers.add_parser('bdev_nvme_cuse_unregister',
                              help='Unregister CUSE devices on NVMe controller')
    p.add_argument('-n', '--name',
                   help='Name of the NVMe controller. Example: Nvme0', required=True)
    p.set_defaults(func=bdev_nvme_cuse_unregister)

    def bdev_zone_block_create(args):
        print_json(rpc.bdev.bdev_zone_block_create(args.client,
                                                   name=args.name,
                                                   base_bdev=args.base_bdev,
                                                   zone_capacity=args.zone_capacity,
                                                   optimal_open_zones=args.optimal_open_zones))

    p = subparsers.add_parser('bdev_zone_block_create',
                              help='Create virtual zone namespace device with block device backend')
    p.add_argument('-b', '--name', help="Name of the zone device", required=True)
    p.add_argument('-n', '--base-bdev', help='Name of underlying, non-zoned bdev', required=True)
    p.add_argument('-z', '--zone-capacity', help='Surfaced zone capacity in blocks', type=int, required=True)
    p.add_argument('-o', '--optimal-open-zones', help='Number of zones required to reach optimal write speed', type=int, required=True)
    p.set_defaults(func=bdev_zone_block_create)

    def bdev_zone_block_delete(args):
        rpc.bdev.bdev_zone_block_delete(args.client,
                                        name=args.name)

    p = subparsers.add_parser('bdev_zone_block_delete', help='Delete a virtual zone namespace device')
    p.add_argument('name', help='Virtual zone bdev name')
    p.set_defaults(func=bdev_zone_block_delete)

    def bdev_rbd_register_cluster(args):
        config_param = None
        if args.config_param:
            config_param = {}
            for entry in args.config_param:
                parts = entry.split('=', 1)
                if len(parts) != 2:
                    raise Exception('--config %s not in key=value form' % entry)
                config_param[parts[0]] = parts[1]
        print_json(rpc.bdev.bdev_rbd_register_cluster(args.client,
                                                      name=args.name,
                                                      user=args.user,
                                                      config_param=config_param,
                                                      config_file=args.config_file,
                                                      key_file=args.key_file,
                                                      core_mask=args.core_mask))

    p = subparsers.add_parser('bdev_rbd_register_cluster',
                              help='Add a Rados cluster with ceph rbd backend')
    p.add_argument('name', help="Name of the Rados cluster only known to rbd bdev")
    p.add_argument('--user', help="Ceph user name (i.e. admin, not client.admin)", required=False)
    p.add_argument('--config-param', action='append', metavar='key=value',
                   help="adds a key=value configuration option for rados_conf_set (default: rely on config file)")
    p.add_argument('--config-file', help="The file path of the Rados configuration file", required=False)
    p.add_argument('--key-file', help="The file path of the Rados keyring file", required=False)
    p.add_argument('--core-mask', help="Set core mask for librbd IO context threads", required=False)
    p.set_defaults(func=bdev_rbd_register_cluster)

    def bdev_rbd_unregister_cluster(args):
        rpc.bdev.bdev_rbd_unregister_cluster(args.client, name=args.name)

    p = subparsers.add_parser('bdev_rbd_unregister_cluster',
                              help='Unregister a Rados cluster object')
    p.add_argument('name', help='Name of the Rados Cluster only known to rbd bdev')
    p.set_defaults(func=bdev_rbd_unregister_cluster)

    def bdev_rbd_get_clusters_info(args):
        print_json(rpc.bdev.bdev_rbd_get_clusters_info(args.client, name=args.name))

    p = subparsers.add_parser('bdev_rbd_get_clusters_info',
                              help='Display registered Rados Cluster names and related info')
    p.add_argument('-b', '--name', help="Name of the registered Rados Cluster Name. Example: Cluster1", required=False)
    p.set_defaults(func=bdev_rbd_get_clusters_info)

    def bdev_rbd_create(args):
        config = None
        if args.config:
            config = {}
            for entry in args.config:
                parts = entry.split('=', 1)
                if len(parts) != 2:
                    raise Exception('--config %s not in key=value form' % entry)
                config[parts[0]] = parts[1]
        print_json(rpc.bdev.bdev_rbd_create(args.client,
                                            name=args.name,
                                            user=args.user,
                                            config=config,
                                            pool_name=args.pool_name,
                                            rbd_name=args.rbd_name,
                                            block_size=args.block_size,
                                            cluster_name=args.cluster_name,
                                            uuid=args.uuid))

    p = subparsers.add_parser('bdev_rbd_create', help='Add a bdev with ceph rbd backend')
    p.add_argument('-b', '--name', help="Name of the bdev", required=False)
    p.add_argument('--user', help="Ceph user name (i.e. admin, not client.admin)", required=False)
    p.add_argument('--config', action='append', metavar='key=value',
                   help="adds a key=value configuration option for rados_conf_set (default: rely on config file)")
    p.add_argument('pool_name', help='rbd pool name')
    p.add_argument('rbd_name', help='rbd image name')
    p.add_argument('block_size', help='rbd block size', type=int)
    p.add_argument('-c', '--cluster-name', help="cluster name to identify the Rados cluster", required=False)
    p.add_argument('-u', '--uuid', help="UUID of the bdev")
    p.set_defaults(func=bdev_rbd_create)

    def bdev_rbd_delete(args):
        rpc.bdev.bdev_rbd_delete(args.client,
                                 name=args.name)

    p = subparsers.add_parser('bdev_rbd_delete', help='Delete a rbd bdev')
    p.add_argument('name', help='rbd bdev name')
    p.set_defaults(func=bdev_rbd_delete)

    def bdev_rbd_resize(args):
        print_json(rpc.bdev.bdev_rbd_resize(args.client,
                                            name=args.name,
                                            new_size=int(args.new_size)))

    p = subparsers.add_parser('bdev_rbd_resize',
                              help='Resize a rbd bdev')
    p.add_argument('name', help='rbd bdev name')
    p.add_argument('new_size', help='new bdev size for resize operation. The unit is MiB')
    p.set_defaults(func=bdev_rbd_resize)

    def bdev_delay_create(args):
        print_json(rpc.bdev.bdev_delay_create(args.client,
                                              base_bdev_name=args.base_bdev_name,
                                              name=args.name,
                                              uuid=args.uuid,
                                              avg_read_latency=args.avg_read_latency,
                                              p99_read_latency=args.nine_nine_read_latency,
                                              avg_write_latency=args.avg_write_latency,
                                              p99_write_latency=args.nine_nine_write_latency))

    p = subparsers.add_parser('bdev_delay_create',
                              help='Add a delay bdev on existing bdev')
    p.add_argument('-b', '--base-bdev-name', help="Name of the existing bdev", required=True)
    p.add_argument('-d', '--name', help="Name of the delay bdev", required=True)
    p.add_argument('-u', '--uuid', help='UUID of the bdev (optional)')
    p.add_argument('-r', '--avg-read-latency',
                   help="Average latency to apply before completing read ops (in microseconds)", required=True, type=int)
    p.add_argument('-t', '--nine-nine-read-latency',
                   help="latency to apply to 1 in 100 read ops (in microseconds)", required=True, type=int)
    p.add_argument('-w', '--avg-write-latency',
                   help="Average latency to apply before completing write ops (in microseconds)", required=True, type=int)
    p.add_argument('-n', '--nine-nine-write-latency',
                   help="latency to apply to 1 in 100 write ops (in microseconds)", required=True, type=int)
    p.set_defaults(func=bdev_delay_create)

    def bdev_delay_delete(args):
        rpc.bdev.bdev_delay_delete(args.client,
                                   name=args.name)

    p = subparsers.add_parser('bdev_delay_delete', help='Delete a delay bdev')
    p.add_argument('name', help='delay bdev name')
    p.set_defaults(func=bdev_delay_delete)

    def bdev_delay_update_latency(args):
        print_json(rpc.bdev.bdev_delay_update_latency(args.client,
                                                      delay_bdev_name=args.delay_bdev_name,
                                                      latency_type=args.latency_type,
                                                      latency_us=args.latency_us))
    p = subparsers.add_parser('bdev_delay_update_latency',
                              help='Update one of the latency values for a given delay bdev')
    p.add_argument('delay_bdev_name', help='The name of the given delay bdev')
    p.add_argument('latency_type', help='one of: avg_read, avg_write, p99_read, p99_write. No other values accepted.')
    p.add_argument('latency_us', help='new latency value in microseconds.', type=int)
    p.set_defaults(func=bdev_delay_update_latency)

    def bdev_error_create(args):
        print_json(rpc.bdev.bdev_error_create(args.client,
                                              base_name=args.base_name,
                                              uuid=args.uuid))

    p = subparsers.add_parser('bdev_error_create', help='Add bdev with error injection backend')
    p.add_argument('base_name', help='base bdev name')
    p.add_argument('--uuid', help='UUID for this bdev', required=False)
    p.set_defaults(func=bdev_error_create)

    def bdev_error_delete(args):
        rpc.bdev.bdev_error_delete(args.client,
                                   name=args.name)

    p = subparsers.add_parser('bdev_error_delete', help='Delete an error bdev')
    p.add_argument('name', help='error bdev name')
    p.set_defaults(func=bdev_error_delete)

    def bdev_iscsi_set_options(args):
        rpc.bdev.bdev_iscsi_set_options(args.client,
                                        timeout_sec=args.timeout_sec)

    p = subparsers.add_parser('bdev_iscsi_set_options', help='Set options for the bdev iscsi type.')
    p.add_argument('-t', '--timeout-sec', help="Timeout for command, in seconds, if 0, don't track timeout.", type=int)
    p.set_defaults(func=bdev_iscsi_set_options)

    def bdev_iscsi_create(args):
        print_json(rpc.bdev.bdev_iscsi_create(args.client,
                                              name=args.name,
                                              url=args.url,
                                              initiator_iqn=args.initiator_iqn))

    p = subparsers.add_parser('bdev_iscsi_create',
                              help='Add bdev with iSCSI initiator backend')
    p.add_argument('-b', '--name', help="Name of the bdev", required=True)
    p.add_argument('-i', '--initiator-iqn', help="Initiator IQN", required=True)
    p.add_argument('--url', help="iSCSI Lun URL", required=True)
    p.set_defaults(func=bdev_iscsi_create)

    def bdev_iscsi_delete(args):
        rpc.bdev.bdev_iscsi_delete(args.client,
                                   name=args.name)

    p = subparsers.add_parser('bdev_iscsi_delete', help='Delete an iSCSI bdev')
    p.add_argument('name', help='iSCSI bdev name')
    p.set_defaults(func=bdev_iscsi_delete)

    def bdev_passthru_create(args):
        print_json(rpc.bdev.bdev_passthru_create(args.client,
                                                 base_bdev_name=args.base_bdev_name,
                                                 name=args.name))

    p = subparsers.add_parser('bdev_passthru_create', help='Add a pass through bdev on existing bdev')
    p.add_argument('-b', '--base-bdev-name', help="Name of the existing bdev", required=True)
    p.add_argument('-p', '--name', help="Name of the pass through bdev", required=True)
    p.set_defaults(func=bdev_passthru_create)

    def bdev_passthru_delete(args):
        rpc.bdev.bdev_passthru_delete(args.client,
                                      name=args.name)

    p = subparsers.add_parser('bdev_passthru_delete', help='Delete a pass through bdev')
    p.add_argument('name', help='pass through bdev name')
    p.set_defaults(func=bdev_passthru_delete)

    def bdev_get_bdevs(args):
        print_dict(rpc.bdev.bdev_get_bdevs(args.client,
                                           name=args.name, timeout=args.timeout_ms))

    p = subparsers.add_parser('bdev_get_bdevs',
                              help='Display current blockdev list or required blockdev')
    p.add_argument('-b', '--name', help="Name of the Blockdev. Example: Nvme0n1", required=False)
    p.add_argument('-t', '--timeout-ms', help="""Time in ms to wait for the bdev to appear (only used
    with the -b|--name option). The default timeout is 0, meaning the RPC returns immediately
    whether the bdev exists or not.""",
                   type=int, required=False)
    p.set_defaults(func=bdev_get_bdevs)

    def bdev_get_iostat(args):
        print_dict(rpc.bdev.bdev_get_iostat(args.client,
                                            name=args.name,
                                            per_channel=args.per_channel))

    p = subparsers.add_parser('bdev_get_iostat',
                              help='Display current I/O statistics of all the blockdevs or specified blockdev.')
    p.add_argument('-b', '--name', help="Name of the Blockdev. Example: Nvme0n1", required=False)
    p.add_argument('-c', '--per-channel', default=False, dest='per_channel', help='Display per channel IO stats for specified device',
                   action='store_true', required=False)
    p.set_defaults(func=bdev_get_iostat)

    def bdev_reset_iostat(args):
        rpc.bdev.bdev_reset_iostat(args.client, name=args.name, mode=args.mode)

    p = subparsers.add_parser('bdev_reset_iostat',
                              help='Reset I/O statistics of all the blockdevs or specified blockdev.')
    p.add_argument('-b', '--name', help="Name of the Blockdev. Example: Nvme0n1", required=False)
    p.add_argument('-m', '--mode', help="Mode to reset I/O statistics", choices=['all', 'maxmin'], required=False)
    p.set_defaults(func=bdev_reset_iostat)

    def bdev_enable_histogram(args):
        rpc.bdev.bdev_enable_histogram(args.client, name=args.name, enable=args.enable)

    p = subparsers.add_parser('bdev_enable_histogram',
                              help='Enable or disable histogram for specified bdev')
    p.add_argument('-e', '--enable', default=True, dest='enable', action='store_true', help='Enable histograms on specified device')
    p.add_argument('-d', '--disable', dest='enable', action='store_false', help='Disable histograms on specified device')
    p.add_argument('name', help='bdev name')
    p.set_defaults(func=bdev_enable_histogram)

    def bdev_get_histogram(args):
        print_dict(rpc.bdev.bdev_get_histogram(args.client, name=args.name))

    p = subparsers.add_parser('bdev_get_histogram',
                              help='Get histogram for specified bdev')
    p.add_argument('name', help='bdev name')
    p.set_defaults(func=bdev_get_histogram)

    def bdev_set_qd_sampling_period(args):
        rpc.bdev.bdev_set_qd_sampling_period(args.client,
                                             name=args.name,
                                             period=args.period)

    p = subparsers.add_parser('bdev_set_qd_sampling_period',
                              help="Enable or disable tracking of a bdev's queue depth.")
    p.add_argument('name', help='Blockdev name. Example: Malloc0')
    p.add_argument('period', help='Period with which to poll the block device queue depth in microseconds.'
                   ' If set to 0, polling will be disabled.',
                   type=int)
    p.set_defaults(func=bdev_set_qd_sampling_period)

    def bdev_set_qos_limit(args):
        rpc.bdev.bdev_set_qos_limit(args.client,
                                    name=args.name,
                                    rw_ios_per_sec=args.rw_ios_per_sec,
                                    rw_mbytes_per_sec=args.rw_mbytes_per_sec,
                                    r_mbytes_per_sec=args.r_mbytes_per_sec,
                                    w_mbytes_per_sec=args.w_mbytes_per_sec)

    p = subparsers.add_parser('bdev_set_qos_limit',
                              help='Set QoS rate limit on a blockdev')
    p.add_argument('name', help='Blockdev name to set QoS. Example: Malloc0')
    p.add_argument('--rw-ios-per-sec',
                   help='R/W IOs per second limit (>=1000, example: 20000). 0 means unlimited.',
                   type=int, required=False)
    p.add_argument('--rw-mbytes-per-sec',
                   help="R/W megabytes per second limit (>=10, example: 100). 0 means unlimited.",
                   type=int, required=False)
    p.add_argument('--r-mbytes-per-sec',
                   help="Read megabytes per second limit (>=10, example: 100). 0 means unlimited.",
                   type=int, required=False)
    p.add_argument('--w-mbytes-per-sec',
                   help="Write megabytes per second limit (>=10, example: 100). 0 means unlimited.",
                   type=int, required=False)
    p.set_defaults(func=bdev_set_qos_limit)

    def bdev_error_inject_error(args):
        rpc.bdev.bdev_error_inject_error(args.client,
                                         name=args.name,
                                         io_type=args.io_type,
                                         error_type=args.error_type,
                                         num=args.num,
                                         corrupt_offset=args.corrupt_offset,
                                         corrupt_value=args.corrupt_value)

    p = subparsers.add_parser('bdev_error_inject_error', help='bdev inject error')
    p.add_argument('name', help="""the name of the error injection bdev""")
    p.add_argument('io_type', help="""io_type: 'clear' 'read' 'write' 'unmap' 'flush' 'all'""")
    p.add_argument('error_type', help="""error_type: 'failure' 'pending' 'corrupt_data'""")
    p.add_argument(
        '-n', '--num', help='the number of commands you want to fail', type=int)
    p.add_argument(
        '-o', '--corrupt-offset', help='the offset in bytes to xor with corrupt_value', type=int)
    p.add_argument(
        '-v', '--corrupt-value', help='the value for xor (1-255, 0 is invalid)', type=int)
    p.set_defaults(func=bdev_error_inject_error)

    def bdev_nvme_apply_firmware(args):
        print_dict(rpc.bdev.bdev_nvme_apply_firmware(args.client,
                                                     bdev_name=args.bdev_name,
                                                     filename=args.filename))

    p = subparsers.add_parser('bdev_nvme_apply_firmware', help='Download and commit firmware to NVMe device')
    p.add_argument('filename', help='filename of the firmware to download')
    p.add_argument('bdev_name', help='name of the NVMe device')
    p.set_defaults(func=bdev_nvme_apply_firmware)

    def bdev_nvme_get_transport_statistics(args):
        print_dict(rpc.bdev.bdev_nvme_get_transport_statistics(args.client))

    p = subparsers.add_parser('bdev_nvme_get_transport_statistics',
                              help='Get bdev_nvme poll group transport statistics')
    p.set_defaults(func=bdev_nvme_get_transport_statistics)

    def bdev_nvme_get_controller_health_info(args):
        print_dict(rpc.bdev.bdev_nvme_get_controller_health_info(args.client,
                                                                 name=args.name))

    p = subparsers.add_parser('bdev_nvme_get_controller_health_info',
                              help='Display health log of the required NVMe bdev controller.')
    p.add_argument('-c', '--name', help="Name of the NVMe bdev controller. Example: Nvme0", required=True)
    p.set_defaults(func=bdev_nvme_get_controller_health_info)

    # iSCSI
    def iscsi_set_options(args):
        rpc.iscsi.iscsi_set_options(
            args.client,
            auth_file=args.auth_file,
            node_base=args.node_base,
            nop_timeout=args.nop_timeout,
            nop_in_interval=args.nop_in_interval,
            disable_chap=args.disable_chap,
            require_chap=args.require_chap,
            mutual_chap=args.mutual_chap,
            chap_group=args.chap_group,
            max_sessions=args.max_sessions,
            max_queue_depth=args.max_queue_depth,
            max_connections_per_session=args.max_connections_per_session,
            default_time2wait=args.default_time2wait,
            default_time2retain=args.default_time2retain,
            first_burst_length=args.first_burst_length,
            immediate_data=args.immediate_data,
            error_recovery_level=args.error_recovery_level,
            allow_duplicated_isid=args.allow_duplicated_isid,
            max_large_datain_per_connection=args.max_large_datain_per_connection,
            max_r2t_per_connection=args.max_r2t_per_connection,
            pdu_pool_size=args.pdu_pool_size,
            immediate_data_pool_size=args.immediate_data_pool_size,
            data_out_pool_size=args.data_out_pool_size)

    p = subparsers.add_parser('iscsi_set_options',
                              help="""Set options of iSCSI subsystem""")
    p.add_argument('-f', '--auth-file', help='Path to CHAP shared secret file')
    p.add_argument('-b', '--node-base', help='Prefix of the name of iSCSI target node')
    p.add_argument('-o', '--nop-timeout', help='Timeout in seconds to nop-in request to the initiator', type=int)
    p.add_argument('-n', '--nop-in-interval', help='Time interval in secs between nop-in requests by the target', type=int)
    p.add_argument('-d', '--disable-chap', help="""CHAP for discovery session should be disabled.
    *** Mutually exclusive with --require-chap""", action='store_true')
    p.add_argument('-r', '--require-chap', help="""CHAP for discovery session should be required.
    *** Mutually exclusive with --disable-chap""", action='store_true')
    p.add_argument('-m', '--mutual-chap', help='CHAP for discovery session should be mutual', action='store_true')
    p.add_argument('-g', '--chap-group', help="""Authentication group ID for discovery session.
    *** Authentication group must be precreated ***""", type=int)
    p.add_argument('-a', '--max-sessions', help='Maximum number of sessions in the host.', type=int)
    p.add_argument('-q', '--max-queue-depth', help='Max number of outstanding I/Os per queue.', type=int)
    p.add_argument('-c', '--max-connections-per-session', help='Negotiated parameter, MaxConnections.', type=int)
    p.add_argument('-w', '--default-time2wait', help='Negotiated parameter, DefaultTime2Wait.', type=int)
    p.add_argument('-v', '--default-time2retain', help='Negotiated parameter, DefaultTime2Retain.', type=int)
    p.add_argument('-s', '--first-burst-length', help='Negotiated parameter, FirstBurstLength.', type=int)
    p.add_argument('-i', '--immediate-data', help='Negotiated parameter, ImmediateData.', action='store_true')
    p.add_argument('-l', '--error-recovery-level', help='Negotiated parameter, ErrorRecoveryLevel', type=int)
    p.add_argument('-p', '--allow-duplicated-isid', help='Allow duplicated initiator session ID.', action='store_true')
    p.add_argument('-x', '--max-large-datain-per-connection', help='Max number of outstanding split read I/Os per connection', type=int)
    p.add_argument('-k', '--max-r2t-per-connection', help='Max number of outstanding R2Ts per connection', type=int)
    p.add_argument('-u', '--pdu-pool-size', help='Number of PDUs in the pool', type=int)
    p.add_argument('-j', '--immediate-data-pool-size', help='Number of immediate data buffers in the pool', type=int)
    p.add_argument('-z', '--data-out-pool-size', help='Number of data out buffers in the pool', type=int)
    p.set_defaults(func=iscsi_set_options)

    def iscsi_set_discovery_auth(args):
        rpc.iscsi.iscsi_set_discovery_auth(
            args.client,
            disable_chap=args.disable_chap,
            require_chap=args.require_chap,
            mutual_chap=args.mutual_chap,
            chap_group=args.chap_group)

    p = subparsers.add_parser('iscsi_set_discovery_auth',
                              help="""Set CHAP authentication for discovery session.""")
    p.add_argument('-d', '--disable-chap', help="""CHAP for discovery session should be disabled.
    *** Mutually exclusive with --require-chap""", action='store_true')
    p.add_argument('-r', '--require-chap', help="""CHAP for discovery session should be required.
    *** Mutually exclusive with --disable-chap""", action='store_true')
    p.add_argument('-m', '--mutual-chap', help='CHAP for discovery session should be mutual', action='store_true')
    p.add_argument('-g', '--chap-group', help="""Authentication group ID for discovery session.
    *** Authentication group must be precreated ***""", type=int)
    p.set_defaults(func=iscsi_set_discovery_auth)

    def iscsi_create_auth_group(args):
        secrets = None
        if args.secrets:
            secrets = [dict(u.split(":") for u in a.split(" ")) for a in args.secrets.split(",")]

        rpc.iscsi.iscsi_create_auth_group(args.client, tag=args.tag, secrets=secrets)

    p = subparsers.add_parser('iscsi_create_auth_group',
                              help='Create authentication group for CHAP authentication.')
    p.add_argument('tag', help='Authentication group tag (unique, integer > 0).', type=int)
    p.add_argument('-c', '--secrets', help="""Comma-separated list of CHAP secrets
<user:user_name secret:chap_secret muser:mutual_user_name msecret:mutual_chap_secret> enclosed in quotes.
Format: 'user:u1 secret:s1 muser:mu1 msecret:ms1,user:u2 secret:s2 muser:mu2 msecret:ms2'""", required=False)
    p.set_defaults(func=iscsi_create_auth_group)

    def iscsi_delete_auth_group(args):
        rpc.iscsi.iscsi_delete_auth_group(args.client, tag=args.tag)

    p = subparsers.add_parser('iscsi_delete_auth_group',
                              help='Delete an authentication group.')
    p.add_argument('tag', help='Authentication group tag', type=int)
    p.set_defaults(func=iscsi_delete_auth_group)

    def iscsi_auth_group_add_secret(args):
        rpc.iscsi.iscsi_auth_group_add_secret(
            args.client,
            tag=args.tag,
            user=args.user,
            secret=args.secret,
            muser=args.muser,
            msecret=args.msecret)

    p = subparsers.add_parser('iscsi_auth_group_add_secret',
                              help='Add a secret to an authentication group.')
    p.add_argument('tag', help='Authentication group tag', type=int)
    p.add_argument('-u', '--user', help='User name for one-way CHAP authentication', required=True)
    p.add_argument('-s', '--secret', help='Secret for one-way CHAP authentication', required=True)
    p.add_argument('-m', '--muser', help='User name for mutual CHAP authentication')
    p.add_argument('-r', '--msecret', help='Secret for mutual CHAP authentication')
    p.set_defaults(func=iscsi_auth_group_add_secret)

    def iscsi_auth_group_remove_secret(args):
        rpc.iscsi.iscsi_auth_group_remove_secret(args.client, tag=args.tag, user=args.user)

    p = subparsers.add_parser('iscsi_auth_group_remove_secret',
                              help='Remove a secret from an authentication group.')
    p.add_argument('tag', help='Authentication group tag', type=int)
    p.add_argument('-u', '--user', help='User name for one-way CHAP authentication', required=True)
    p.set_defaults(func=iscsi_auth_group_remove_secret)

    def iscsi_get_auth_groups(args):
        print_dict(rpc.iscsi.iscsi_get_auth_groups(args.client))

    p = subparsers.add_parser('iscsi_get_auth_groups',
                              help='Display current authentication group configuration')
    p.set_defaults(func=iscsi_get_auth_groups)

    def iscsi_get_portal_groups(args):
        print_dict(rpc.iscsi.iscsi_get_portal_groups(args.client))

    p = subparsers.add_parser('iscsi_get_portal_groups', help='Display current portal group configuration')
    p.set_defaults(func=iscsi_get_portal_groups)

    def iscsi_get_initiator_groups(args):
        print_dict(rpc.iscsi.iscsi_get_initiator_groups(args.client))

    p = subparsers.add_parser('iscsi_get_initiator_groups',
                              help='Display current initiator group configuration')
    p.set_defaults(func=iscsi_get_initiator_groups)

    def iscsi_get_target_nodes(args):
        print_dict(rpc.iscsi.iscsi_get_target_nodes(args.client))

    p = subparsers.add_parser('iscsi_get_target_nodes', help='Display target nodes')
    p.set_defaults(func=iscsi_get_target_nodes)

    def iscsi_create_target_node(args):
        luns = []
        for u in args.bdev_name_id_pairs.strip().split(" "):
            bdev_name, lun_id = u.split(":")
            luns.append({"bdev_name": bdev_name, "lun_id": int(lun_id)})

        pg_ig_maps = []
        for u in args.pg_ig_mappings.strip().split(" "):
            pg, ig = u.split(":")
            pg_ig_maps.append({"pg_tag": int(pg), "ig_tag": int(ig)})

        rpc.iscsi.iscsi_create_target_node(
            args.client,
            luns=luns,
            pg_ig_maps=pg_ig_maps,
            name=args.name,
            alias_name=args.alias_name,
            queue_depth=args.queue_depth,
            chap_group=args.chap_group,
            disable_chap=args.disable_chap,
            require_chap=args.require_chap,
            mutual_chap=args.mutual_chap,
            header_digest=args.header_digest,
            data_digest=args.data_digest)

    p = subparsers.add_parser('iscsi_create_target_node', help='Add a target node')
    p.add_argument('name', help='Target node name (ASCII)')
    p.add_argument('alias_name', help='Target node alias name (ASCII)')
    p.add_argument('bdev_name_id_pairs', help="""Whitespace-separated list of <bdev name:LUN ID> pairs enclosed
    in quotes.  Format:  'bdev_name0:id0 bdev_name1:id1' etc
    Example: 'Malloc0:0 Malloc1:1 Malloc5:2'
    *** The bdevs must pre-exist ***
    *** LUN0 (id = 0) is required ***
    *** bdevs names cannot contain space or colon characters ***""")
    p.add_argument('pg_ig_mappings', help="""List of (Portal_Group_Tag:Initiator_Group_Tag) mappings
    Whitespace separated, quoted, mapping defined with colon
    separated list of "tags" (int > 0)
    Example: '1:1 2:2 2:1'
    *** The Portal/Initiator Groups must be precreated ***""")
    p.add_argument('queue_depth', help='Desired target queue depth', type=int)
    p.add_argument('-g', '--chap-group', help="""Authentication group ID for this target node.
    *** Authentication group must be precreated ***""", type=int)
    p.add_argument('-d', '--disable-chap', help="""CHAP authentication should be disabled for this target node.
    *** Mutually exclusive with --require-chap ***""", action='store_true')
    p.add_argument('-r', '--require-chap', help="""CHAP authentication should be required for this target node.
    *** Mutually exclusive with --disable-chap ***""", action='store_true')
    p.add_argument(
        '-m', '--mutual-chap', help='CHAP authentication should be mutual/bidirectional.', action='store_true')
    p.add_argument('-H', '--header-digest',
                   help='Header Digest should be required for this target node.', action='store_true')
    p.add_argument('-D', '--data-digest',
                   help='Data Digest should be required for this target node.', action='store_true')
    p.set_defaults(func=iscsi_create_target_node)

    def iscsi_target_node_add_lun(args):
        rpc.iscsi.iscsi_target_node_add_lun(
            args.client,
            name=args.name,
            bdev_name=args.bdev_name,
            lun_id=args.lun_id)

    p = subparsers.add_parser('iscsi_target_node_add_lun',
                              help='Add LUN to the target node')
    p.add_argument('name', help='Target node name (ASCII)')
    p.add_argument('bdev_name', help="""bdev name enclosed in quotes.
    *** bdev name cannot contain space or colon characters ***""")
    p.add_argument('-i', dest='lun_id', help="""LUN ID (integer >= 0)
    *** If LUN ID is omitted or -1, the lowest free one is assigned ***""", type=int, required=False)
    p.set_defaults(func=iscsi_target_node_add_lun)

    def iscsi_target_node_set_auth(args):
        rpc.iscsi.iscsi_target_node_set_auth(
            args.client,
            name=args.name,
            chap_group=args.chap_group,
            disable_chap=args.disable_chap,
            require_chap=args.require_chap,
            mutual_chap=args.mutual_chap)

    p = subparsers.add_parser('iscsi_target_node_set_auth',
                              help='Set CHAP authentication for the target node')
    p.add_argument('name', help='Target node name (ASCII)')
    p.add_argument('-g', '--chap-group', help="""Authentication group ID for this target node.
    *** Authentication group must be precreated ***""", type=int)
    p.add_argument('-d', '--disable-chap', help="""CHAP authentication should be disabled for this target node.
    *** Mutually exclusive with --require-chap ***""", action='store_true')
    p.add_argument('-r', '--require-chap', help="""CHAP authentication should be required for this target node.
    *** Mutually exclusive with --disable-chap ***""", action='store_true')
    p.add_argument('-m', '--mutual-chap', help='CHAP authentication should be mutual/bidirectional.',
                   action='store_true')
    p.set_defaults(func=iscsi_target_node_set_auth)

    def iscsi_target_node_add_pg_ig_maps(args):
        pg_ig_maps = []
        for u in args.pg_ig_mappings.strip().split(" "):
            pg, ig = u.split(":")
            pg_ig_maps.append({"pg_tag": int(pg), "ig_tag": int(ig)})
        rpc.iscsi.iscsi_target_node_add_pg_ig_maps(
            args.client,
            pg_ig_maps=pg_ig_maps,
            name=args.name)

    p = subparsers.add_parser('iscsi_target_node_add_pg_ig_maps',
                              help='Add PG-IG maps to the target node')
    p.add_argument('name', help='Target node name (ASCII)')
    p.add_argument('pg_ig_mappings', help="""List of (Portal_Group_Tag:Initiator_Group_Tag) mappings
    Whitespace separated, quoted, mapping defined with colon
    separated list of "tags" (int > 0)
    Example: '1:1 2:2 2:1'
    *** The Portal/Initiator Groups must be precreated ***""")
    p.set_defaults(func=iscsi_target_node_add_pg_ig_maps)

    def iscsi_target_node_remove_pg_ig_maps(args):
        pg_ig_maps = []
        for u in args.pg_ig_mappings.strip().split(" "):
            pg, ig = u.split(":")
            pg_ig_maps.append({"pg_tag": int(pg), "ig_tag": int(ig)})
        rpc.iscsi.iscsi_target_node_remove_pg_ig_maps(
            args.client, pg_ig_maps=pg_ig_maps, name=args.name)

    p = subparsers.add_parser('iscsi_target_node_remove_pg_ig_maps',
                              help='Delete PG-IG maps from the target node')
    p.add_argument('name', help='Target node name (ASCII)')
    p.add_argument('pg_ig_mappings', help="""List of (Portal_Group_Tag:Initiator_Group_Tag) mappings
    Whitespace separated, quoted, mapping defined with colon
    separated list of "tags" (int > 0)
    Example: '1:1 2:2 2:1'
    *** The Portal/Initiator Groups must be precreated ***""")
    p.set_defaults(func=iscsi_target_node_remove_pg_ig_maps)

    def iscsi_target_node_set_redirect(args):
        rpc.iscsi.iscsi_target_node_set_redirect(
            args.client,
            name=args.name,
            pg_tag=args.pg_tag,
            redirect_host=args.redirect_host,
            redirect_port=args.redirect_port)

    p = subparsers.add_parser('iscsi_target_node_set_redirect',
                              help="""Update redirect portal of the public portal group for the target node.
    Omit redirect host and port to clear previously set redirect settings.""")
    p.add_argument('name', help='Target node name (ASCII)')
    p.add_argument('pg_tag', help='Portal group tag (unique, integer > 0)', type=int)
    p.add_argument('-a', '--redirect-host', help='Numeric IP address for redirect portal', required=False)
    p.add_argument('-p', '--redirect-port', help='Numeric TCP port for redirect portal', required=False)
    p.set_defaults(func=iscsi_target_node_set_redirect)

    def iscsi_target_node_request_logout(args):
        rpc.iscsi.iscsi_target_node_request_logout(
            args.client,
            name=args.name,
            pg_tag=args.pg_tag)

    p = subparsers.add_parser('iscsi_target_node_request_logout',
                              help="""For the target node, request connections whose portal group tag
    match to logout, or request all connections if portal group tag is omitted.""")
    p.add_argument('name', help='Target node name (ASCII)')
    p.add_argument('-t', '--pg-tag', help='Portal group tag (unique, integer > 0)', type=int, required=False)
    p.set_defaults(func=iscsi_target_node_request_logout)

    def iscsi_create_portal_group(args):
        portals = []
        for p in args.portal_list.strip().split(' '):
            ip, separator, port_cpumask = p.rpartition(':')
            split_port_cpumask = port_cpumask.split('@')
            if len(split_port_cpumask) == 1:
                port = port_cpumask
                portals.append({'host': ip, 'port': port})
            else:
                port = split_port_cpumask[0]
                cpumask = split_port_cpumask[1]
                portals.append({'host': ip, 'port': port})
                print("WARNING: Specifying a portal group with a CPU mask is no longer supported. Ignoring it.")
        rpc.iscsi.iscsi_create_portal_group(
            args.client,
            portals=portals,
            tag=args.tag,
            private=args.private,
            wait=args.wait)

    p = subparsers.add_parser('iscsi_create_portal_group',
                              help='Add a portal group')
    p.add_argument(
        'tag', help='Portal group tag (unique, integer > 0)', type=int)
    p.add_argument('portal_list', help="""List of portals in host:port format, separated by whitespace
    Example: '192.168.100.100:3260 192.168.100.100:3261 192.168.100.100:3262""")
    p.add_argument('-p', '--private', help="""Public (false) or private (true) portal group.
    Private portal groups do not have their portals returned by a discovery session. A public
    portal group may optionally specify a redirect portal for non-discovery logins. This redirect
    portal must be from a private portal group.""", action='store_true')
    p.add_argument('-w', '--wait', help="""Do not listening on portals until it is started explicitly.
    One major iSCSI initiator may not retry login once it failed. Hence for such initiator, listening
    on portals should be allowed after all associated target nodes are created.""", action='store_true')
    p.set_defaults(func=iscsi_create_portal_group)

    def iscsi_start_portal_group(args):
        rpc.iscsi.iscsi_start_portal_group(args.client, tag=args.tag)

    p = subparsers.add_parser('iscsi_start_portal_group',
                              help='Start listening on portals if it is not started yet.')
    p.add_argument(
        'tag', help='Portal group tag (unique, integer > 0)', type=int)
    p.set_defaults(func=iscsi_start_portal_group)

    def iscsi_create_initiator_group(args):
        initiators = []
        netmasks = []
        for i in args.initiator_list.strip().split(' '):
            initiators.append(i)
        for n in args.netmask_list.strip().split(' '):
            netmasks.append(n)
        rpc.iscsi.iscsi_create_initiator_group(
            args.client,
            tag=args.tag,
            initiators=initiators,
            netmasks=netmasks)

    p = subparsers.add_parser('iscsi_create_initiator_group',
                              help='Add an initiator group')
    p.add_argument(
        'tag', help='Initiator group tag (unique, integer > 0)', type=int)
    p.add_argument('initiator_list', help="""Whitespace-separated list of initiator hostnames or IP addresses,
    enclosed in quotes.  Example: 'ANY' or 'iqn.2016-06.io.spdk:host1 iqn.2016-06.io.spdk:host2'""")
    p.add_argument('netmask_list', help="""Whitespace-separated list of initiator netmasks enclosed in quotes.
    Example: '255.255.0.0 255.248.0.0' etc""")
    p.set_defaults(func=iscsi_create_initiator_group)

    def iscsi_initiator_group_add_initiators(args):
        initiators = None
        netmasks = None
        if args.initiator_list:
            initiators = []
            for i in args.initiator_list.strip().split(' '):
                initiators.append(i)
        if args.netmask_list:
            netmasks = []
            for n in args.netmask_list.strip().split(' '):
                netmasks.append(n)
        rpc.iscsi.iscsi_initiator_group_add_initiators(
            args.client,
            tag=args.tag,
            initiators=initiators,
            netmasks=netmasks)

    p = subparsers.add_parser('iscsi_initiator_group_add_initiators',
                              help='Add initiators to an existing initiator group')
    p.add_argument(
        'tag', help='Initiator group tag (unique, integer > 0)', type=int)
    p.add_argument('-n', dest='initiator_list', help="""Whitespace-separated list of initiator hostnames or IP addresses,
    enclosed in quotes.  This parameter can be omitted.  Example: 'ANY' or
    'iqn.2016-06.io.spdk:host1 iqn.2016-06.io.spdk:host2'""", required=False)
    p.add_argument('-m', dest='netmask_list', help="""Whitespace-separated list of initiator netmasks enclosed in quotes.
    This parameter can be omitted.  Example: '255.255.0.0 255.248.0.0' etc""", required=False)
    p.set_defaults(func=iscsi_initiator_group_add_initiators)

    def iscsi_initiator_group_remove_initiators(args):
        initiators = None
        netmasks = None
        if args.initiator_list:
            initiators = []
            for i in args.initiator_list.strip().split(' '):
                initiators.append(i)
        if args.netmask_list:
            netmasks = []
            for n in args.netmask_list.strip().split(' '):
                netmasks.append(n)
        rpc.iscsi.iscsi_initiator_group_remove_initiators(
            args.client,
            tag=args.tag,
            initiators=initiators,
            netmasks=netmasks)

    p = subparsers.add_parser('iscsi_initiator_group_remove_initiators',
                              help='Delete initiators from an existing initiator group')
    p.add_argument(
        'tag', help='Initiator group tag (unique, integer > 0)', type=int)
    p.add_argument('-n', dest='initiator_list', help="""Whitespace-separated list of initiator hostnames or IP addresses,
    enclosed in quotes.  This parameter can be omitted.  Example: 'ANY' or
    'iqn.2016-06.io.spdk:host1 iqn.2016-06.io.spdk:host2'""", required=False)
    p.add_argument('-m', dest='netmask_list', help="""Whitespace-separated list of initiator netmasks enclosed in quotes.
    This parameter can be omitted.  Example: '255.255.0.0 255.248.0.0' etc""", required=False)
    p.set_defaults(func=iscsi_initiator_group_remove_initiators)

    def iscsi_delete_target_node(args):
        rpc.iscsi.iscsi_delete_target_node(
            args.client, target_node_name=args.target_node_name)

    p = subparsers.add_parser('iscsi_delete_target_node',
                              help='Delete a target node')
    p.add_argument('target_node_name',
                   help='Target node name to be deleted. Example: iqn.2016-06.io.spdk:disk1.')
    p.set_defaults(func=iscsi_delete_target_node)

    def iscsi_delete_portal_group(args):
        rpc.iscsi.iscsi_delete_portal_group(args.client, tag=args.tag)

    p = subparsers.add_parser('iscsi_delete_portal_group',
                              help='Delete a portal group')
    p.add_argument(
        'tag', help='Portal group tag (unique, integer > 0)', type=int)
    p.set_defaults(func=iscsi_delete_portal_group)

    def iscsi_delete_initiator_group(args):
        rpc.iscsi.iscsi_delete_initiator_group(args.client, tag=args.tag)

    p = subparsers.add_parser('iscsi_delete_initiator_group',
                              help='Delete an initiator group')
    p.add_argument(
        'tag', help='Initiator group tag (unique, integer > 0)', type=int)
    p.set_defaults(func=iscsi_delete_initiator_group)

    def iscsi_portal_group_set_auth(args):
        rpc.iscsi.iscsi_portal_group_set_auth(
            args.client,
            tag=args.tag,
            chap_group=args.chap_group,
            disable_chap=args.disable_chap,
            require_chap=args.require_chap,
            mutual_chap=args.mutual_chap)

    p = subparsers.add_parser('iscsi_portal_group_set_auth',
                              help='Set CHAP authentication for discovery sessions specific for the portal group')
    p.add_argument('tag', help='Portal group tag (unique, integer > 0)', type=int)
    p.add_argument('-g', '--chap-group', help="""Authentication group ID for this portal group.
    *** Authentication group must be precreated ***""", type=int)
    p.add_argument('-d', '--disable-chap', help="""CHAP authentication should be disabled for this portal group.
    *** Mutually exclusive with --require-chap ***""", action='store_true')
    p.add_argument('-r', '--require-chap', help="""CHAP authentication should be required for this portal group.
    *** Mutually exclusive with --disable-chap ***""", action='store_true')
    p.add_argument('-m', '--mutual-chap', help='CHAP authentication should be mutual/bidirectional.',
                   action='store_true')
    p.set_defaults(func=iscsi_portal_group_set_auth)

    def iscsi_get_connections(args):
        print_dict(rpc.iscsi.iscsi_get_connections(args.client))

    p = subparsers.add_parser('iscsi_get_connections',
                              help='Display iSCSI connections')
    p.set_defaults(func=iscsi_get_connections)

    def iscsi_get_options(args):
        print_dict(rpc.iscsi.iscsi_get_options(args.client))

    p = subparsers.add_parser('iscsi_get_options',
                              help='Display iSCSI global parameters')
    p.set_defaults(func=iscsi_get_options)

    def scsi_get_devices(args):
        print_dict(rpc.iscsi.scsi_get_devices(args.client))

    p = subparsers.add_parser('scsi_get_devices', help='Display SCSI devices')
    p.set_defaults(func=scsi_get_devices)

    # trace
    def trace_enable_tpoint_group(args):
        rpc.trace.trace_enable_tpoint_group(args.client, name=args.name)

    p = subparsers.add_parser('trace_enable_tpoint_group',
                              help='enable trace on a specific tpoint group')
    p.add_argument(
        'name', help="""trace group name we want to enable in tpoint_group_mask.
        (for example "bdev" for bdev trace group, "all" for all trace groups).""")
    p.set_defaults(func=trace_enable_tpoint_group)

    def trace_disable_tpoint_group(args):
        rpc.trace.trace_disable_tpoint_group(args.client, name=args.name)

    p = subparsers.add_parser('trace_disable_tpoint_group',
                              help='disable trace on a specific tpoint group')
    p.add_argument(
        'name', help="""trace group name we want to disable in tpoint_group_mask.
        (for example "bdev" for bdev trace group, "all" for all trace groups).""")
    p.set_defaults(func=trace_disable_tpoint_group)

    def trace_set_tpoint_mask(args):
        rpc.trace.trace_set_tpoint_mask(args.client, name=args.name, tpoint_mask=args.tpoint_mask)

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
        rpc.trace.trace_clear_tpoint_mask(args.client, name=args.name, tpoint_mask=args.tpoint_mask)

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
        print_dict(rpc.trace.trace_get_tpoint_group_mask(args.client))

    p = subparsers.add_parser('trace_get_tpoint_group_mask', help='get trace point group mask')
    p.set_defaults(func=trace_get_tpoint_group_mask)

    def trace_get_info(args):
        print_dict(rpc.trace.trace_get_info(args.client))

    p = subparsers.add_parser('trace_get_info',
                              help='get name of shared memory file and list of the available trace point groups')
    p.set_defaults(func=trace_get_info)

    # log
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

    # lvol
    def bdev_lvol_create_lvstore(args):
        print_json(rpc.lvol.bdev_lvol_create_lvstore(args.client,
                                                     bdev_name=args.bdev_name,
                                                     lvs_name=args.lvs_name,
                                                     cluster_sz=args.cluster_sz,
                                                     clear_method=args.clear_method,
                                                     num_md_pages_per_cluster_ratio=args.md_pages_per_cluster_ratio))

    p = subparsers.add_parser('bdev_lvol_create_lvstore', help='Add logical volume store on base bdev')
    p.add_argument('bdev_name', help='base bdev name')
    p.add_argument('lvs_name', help='name for lvol store')
    p.add_argument('-c', '--cluster-sz', help='size of cluster (in bytes)', type=int, required=False)
    p.add_argument('--clear-method', help="""Change clear method for data region.
        Available: none, unmap, write_zeroes""", required=False)
    p.add_argument('-m', '--md-pages-per-cluster-ratio', help='reserved metadata pages for each cluster', type=int, required=False)
    p.set_defaults(func=bdev_lvol_create_lvstore)

    def bdev_lvol_rename_lvstore(args):
        rpc.lvol.bdev_lvol_rename_lvstore(args.client,
                                          old_name=args.old_name,
                                          new_name=args.new_name)

    p = subparsers.add_parser('bdev_lvol_rename_lvstore', help='Change logical volume store name')
    p.add_argument('old_name', help='old name')
    p.add_argument('new_name', help='new name')
    p.set_defaults(func=bdev_lvol_rename_lvstore)

    def bdev_lvol_grow_lvstore(args):
        print_dict(rpc.lvol.bdev_lvol_grow_lvstore(args.client,
                                                   uuid=args.uuid,
                                                   lvs_name=args.lvs_name))

    p = subparsers.add_parser('bdev_lvol_grow_lvstore',
                              help='Grow the lvstore size to the underlying bdev size')
    p.add_argument('-u', '--uuid', help='lvol store UUID', required=False)
    p.add_argument('-l', '--lvs-name', help='lvol store name', required=False)
    p.set_defaults(func=bdev_lvol_grow_lvstore)

    def bdev_lvol_create(args):
        print_json(rpc.lvol.bdev_lvol_create(args.client,
                                             lvol_name=args.lvol_name,
                                             size_in_mib=args.size_in_mib,
                                             thin_provision=args.thin_provision,
                                             clear_method=args.clear_method,
                                             uuid=args.uuid,
                                             lvs_name=args.lvs_name))

    p = subparsers.add_parser('bdev_lvol_create', help='Add a bdev with an logical volume backend')
    p.add_argument('-u', '--uuid', help='lvol store UUID', required=False)
    p.add_argument('-l', '--lvs-name', help='lvol store name', required=False)
    p.add_argument('-t', '--thin-provision', action='store_true', help='create lvol bdev as thin provisioned')
    p.add_argument('-c', '--clear-method', help="""Change default data clusters clear method.
        Available: none, unmap, write_zeroes""", required=False)
    p.add_argument('lvol_name', help='name for this lvol')
    p.add_argument('size_in_mib', help='size in MiB for this bdev', type=int)
    p.set_defaults(func=bdev_lvol_create)

    def bdev_lvol_snapshot(args):
        print_json(rpc.lvol.bdev_lvol_snapshot(args.client,
                                               lvol_name=args.lvol_name,
                                               snapshot_name=args.snapshot_name))

    p = subparsers.add_parser('bdev_lvol_snapshot', help='Create a snapshot of an lvol bdev')
    p.add_argument('lvol_name', help='lvol bdev name')
    p.add_argument('snapshot_name', help='lvol snapshot name')
    p.set_defaults(func=bdev_lvol_snapshot)

    def bdev_lvol_clone(args):
        print_json(rpc.lvol.bdev_lvol_clone(args.client,
                                            snapshot_name=args.snapshot_name,
                                            clone_name=args.clone_name))

    p = subparsers.add_parser('bdev_lvol_clone', help='Create a clone of an lvol snapshot')
    p.add_argument('snapshot_name', help='lvol snapshot name')
    p.add_argument('clone_name', help='lvol clone name')
    p.set_defaults(func=bdev_lvol_clone)

    def bdev_lvol_clone_bdev(args):
        print_json(rpc.lvol.bdev_lvol_clone_bdev(args.client,
                                                 bdev=args.bdev,
                                                 lvs_name=args.lvs_name,
                                                 clone_name=args.clone_name))

    p = subparsers.add_parser('bdev_lvol_clone_bdev',
                              help='Create a clone of a non-lvol bdev')
    p.add_argument('bdev', help='bdev to clone')
    p.add_argument('lvs_name', help='logical volume store name')
    p.add_argument('clone_name', help='lvol clone name')
    p.set_defaults(func=bdev_lvol_clone_bdev)

    def bdev_lvol_rename(args):
        rpc.lvol.bdev_lvol_rename(args.client,
                                  old_name=args.old_name,
                                  new_name=args.new_name)

    p = subparsers.add_parser('bdev_lvol_rename', help='Change lvol bdev name')
    p.add_argument('old_name', help='lvol bdev name')
    p.add_argument('new_name', help='new lvol name')
    p.set_defaults(func=bdev_lvol_rename)

    def bdev_lvol_inflate(args):
        rpc.lvol.bdev_lvol_inflate(args.client,
                                   name=args.name)

    p = subparsers.add_parser('bdev_lvol_inflate', help='Make thin provisioned lvol a thick provisioned lvol')
    p.add_argument('name', help='lvol bdev name')
    p.set_defaults(func=bdev_lvol_inflate)

    def bdev_lvol_decouple_parent(args):
        rpc.lvol.bdev_lvol_decouple_parent(args.client,
                                           name=args.name)

    p = subparsers.add_parser('bdev_lvol_decouple_parent', help='Decouple parent of lvol')
    p.add_argument('name', help='lvol bdev name')
    p.set_defaults(func=bdev_lvol_decouple_parent)

    def bdev_lvol_resize(args):
        rpc.lvol.bdev_lvol_resize(args.client,
                                  name=args.name,
                                  size_in_mib=args.size_in_mib)

    p = subparsers.add_parser('bdev_lvol_resize', help='Resize existing lvol bdev')
    p.add_argument('name', help='lvol bdev name')
    p.add_argument('size_in_mib', help='new size in MiB for this bdev', type=int)
    p.set_defaults(func=bdev_lvol_resize)

    def bdev_lvol_set_read_only(args):
        rpc.lvol.bdev_lvol_set_read_only(args.client,
                                         name=args.name)

    p = subparsers.add_parser('bdev_lvol_set_read_only', help='Mark lvol bdev as read only')
    p.add_argument('name', help='lvol bdev name')
    p.set_defaults(func=bdev_lvol_set_read_only)

    def bdev_lvol_delete(args):
        rpc.lvol.bdev_lvol_delete(args.client,
                                  name=args.name)

    p = subparsers.add_parser('bdev_lvol_delete', help='Destroy a logical volume')
    p.add_argument('name', help='lvol bdev name')
    p.set_defaults(func=bdev_lvol_delete)

    def bdev_lvol_delete_lvstore(args):
        rpc.lvol.bdev_lvol_delete_lvstore(args.client,
                                          uuid=args.uuid,
                                          lvs_name=args.lvs_name)

    p = subparsers.add_parser('bdev_lvol_delete_lvstore', help='Destroy an logical volume store')
    p.add_argument('-u', '--uuid', help='lvol store UUID', required=False)
    p.add_argument('-l', '--lvs-name', help='lvol store name', required=False)
    p.set_defaults(func=bdev_lvol_delete_lvstore)

    def bdev_lvol_get_lvstores(args):
        print_dict(rpc.lvol.bdev_lvol_get_lvstores(args.client,
                                                   uuid=args.uuid,
                                                   lvs_name=args.lvs_name))

    p = subparsers.add_parser('bdev_lvol_get_lvstores', help='Display current logical volume store list')
    p.add_argument('-u', '--uuid', help='lvol store UUID', required=False)
    p.add_argument('-l', '--lvs-name', help='lvol store name', required=False)
    p.set_defaults(func=bdev_lvol_get_lvstores)

    def bdev_lvol_get_lvols(args):
        print_dict(rpc.lvol.bdev_lvol_get_lvols(args.client,
                                                lvs_uuid=args.lvs_uuid,
                                                lvs_name=args.lvs_name))

    p = subparsers.add_parser('bdev_lvol_get_lvols', help='Display current logical volume list')
    p.add_argument('-u', '--lvs-uuid', help='only lvols in  lvol store UUID', required=False)
    p.add_argument('-l', '--lvs-name', help='only lvols in lvol store name', required=False)
    p.set_defaults(func=bdev_lvol_get_lvols)

    def bdev_raid_get_bdevs(args):
        print_json(rpc.bdev.bdev_raid_get_bdevs(args.client,
                                                category=args.category))

    p = subparsers.add_parser('bdev_raid_get_bdevs',
                              help="""This is used to list all the raid bdev details based on the input category
    requested. Category should be one of 'all', 'online', 'configuring' or 'offline'. 'all' means all the raid bdevs whether
    they are online or configuring or offline. 'online' is the raid bdev which is registered with bdev layer. 'configuring'
    is the raid bdev which does not have full configuration discovered yet. 'offline' is the raid bdev which is not registered
    with bdev as of now and it has encountered any error or user has requested to offline the raid bdev""")
    p.add_argument('category', help='all or online or configuring or offline')
    p.set_defaults(func=bdev_raid_get_bdevs)

    def bdev_raid_create(args):
        base_bdevs = []
        for u in args.base_bdevs.strip().split(" "):
            base_bdevs.append(u)

        rpc.bdev.bdev_raid_create(args.client,
                                  name=args.name,
                                  strip_size_kb=args.strip_size_kb,
                                  raid_level=args.raid_level,
                                  base_bdevs=base_bdevs,
                                  uuid=args.uuid)
    p = subparsers.add_parser('bdev_raid_create', help='Create new raid bdev')
    p.add_argument('-n', '--name', help='raid bdev name', required=True)
    p.add_argument('-z', '--strip-size-kb', help='strip size in KB', type=int)
    p.add_argument('-r', '--raid-level', help='raid level, raid0, raid1 and a special level concat are supported', required=True)
    p.add_argument('-b', '--base-bdevs', help='base bdevs name, whitespace separated list in quotes', required=True)
    p.add_argument('--uuid', help='UUID for this raid bdev', required=False)
    p.set_defaults(func=bdev_raid_create)

    def bdev_raid_delete(args):
        rpc.bdev.bdev_raid_delete(args.client,
                                  name=args.name)
    p = subparsers.add_parser('bdev_raid_delete', help='Delete existing raid bdev')
    p.add_argument('name', help='raid bdev name')
    p.set_defaults(func=bdev_raid_delete)

    # split
    def bdev_split_create(args):
        print_array(rpc.bdev.bdev_split_create(args.client,
                                               base_bdev=args.base_bdev,
                                               split_count=args.split_count,
                                               split_size_mb=args.split_size_mb))

    p = subparsers.add_parser('bdev_split_create',
                              help="""Add given disk name to split config. If bdev with base_name
    name exist the split bdevs will be created right away, if not split bdevs will be created when base bdev became
    available (during examination process).""")
    p.add_argument('base_bdev', help='base bdev name')
    p.add_argument('-s', '--split-size-mb', help='size in MiB for each bdev', type=int)
    p.add_argument('split_count', help="""Optional - number of split bdevs to create. Total size * split_count must not
    exceed the base bdev size.""", type=int)
    p.set_defaults(func=bdev_split_create)

    def bdev_split_delete(args):
        rpc.bdev.bdev_split_delete(args.client,
                                   base_bdev=args.base_bdev)

    p = subparsers.add_parser('bdev_split_delete', help="""Delete split config with all created splits.""")
    p.add_argument('base_bdev', help='base bdev name')
    p.set_defaults(func=bdev_split_delete)

    # ftl
    def bdev_ftl_create(args):
        print_dict(rpc.bdev.bdev_ftl_create(args.client,
                                            name=args.name,
                                            base_bdev=args.base_bdev,
                                            uuid=args.uuid,
                                            cache=args.cache,
                                            overprovisioning=args.overprovisioning,
                                            l2p_dram_limit=args.l2p_dram_limit,
                                            core_mask=args.core_mask,
                                            fast_shutdown=args.fast_shutdown))

    p = subparsers.add_parser('bdev_ftl_create', help='Add FTL bdev')
    p.add_argument('-b', '--name', help="Name of the bdev", required=True)
    p.add_argument('-d', '--base-bdev', help='Name of bdev used as underlying device',
                   required=True)
    p.add_argument('-u', '--uuid', help='UUID of restored bdev (not applicable when creating new '
                   'instance): e.g. b286d19a-0059-4709-abcd-9f7732b1567d (optional)')
    p.add_argument('-c', '--cache', help='Name of the bdev to be used as a write buffer cache',
                   required=True)
    p.add_argument('--overprovisioning', help='Percentage of device used for relocation, not exposed'
                   ' to user (optional); default 20', type=int)
    p.add_argument('--l2p-dram-limit', help='l2p size that could reside in DRAM (optional); default 2048',
                   type=int)
    p.add_argument('--core-mask', help='CPU core mask - which cores will be used for ftl core thread, '
                   'by default core thread will be set to the main application core (optional)')
    p.add_argument('-f', '--fast-shutdown', help="Enable fast shutdown", action='store_true')
    p.set_defaults(func=bdev_ftl_create)

    def bdev_ftl_load(args):
        print_dict(rpc.bdev.bdev_ftl_load(args.client,
                                          name=args.name,
                                          base_bdev=args.base_bdev,
                                          uuid=args.uuid,
                                          cache=args.cache,
                                          overprovisioning=args.overprovisioning,
                                          l2p_dram_limit=args.l2p_dram_limit,
                                          core_mask=args.core_mask,
                                          fast_shutdown=args.fast_shutdown))

    p = subparsers.add_parser('bdev_ftl_load', help='Load FTL bdev')
    p.add_argument('-b', '--name', help="Name of the bdev", required=True)
    p.add_argument('-d', '--base-bdev', help='Name of bdev used as underlying device',
                   required=True)
    p.add_argument('-u', '--uuid', help='UUID of restored bdev', required=True)
    p.add_argument('-c', '--cache', help='Name of the bdev to be used as a write buffer cache',
                   required=True)
    p.add_argument('--overprovisioning', help='Percentage of device used for relocation, not exposed'
                   ' to user (optional); default 20', type=int)
    p.add_argument('--l2p-dram-limit', help='l2p size that could reside in DRAM (optional); default 2048',
                   type=int)
    p.add_argument('--core-mask', help='CPU core mask - which cores will be used for ftl core thread, '
                   'by default core thread will be set to the main application core (optional)')
    p.add_argument('-f', '--fast-shutdown', help="Enable fast shutdown", action='store_true')
    p.set_defaults(func=bdev_ftl_load)

    def bdev_ftl_unload(args):
        print_dict(rpc.bdev.bdev_ftl_unload(args.client, name=args.name, fast_shutdown=args.fast_shutdown))

    p = subparsers.add_parser('bdev_ftl_unload', help='Unload FTL bdev')
    p.add_argument('-b', '--name', help="Name of the bdev", required=True)
    p.add_argument('-f', '--fast-shutdown', help="Fast shutdown", action='store_true')
    p.set_defaults(func=bdev_ftl_unload)

    def bdev_ftl_delete(args):
        print_dict(rpc.bdev.bdev_ftl_delete(args.client, name=args.name, fast_shutdown=args.fast_shutdown))

    p = subparsers.add_parser('bdev_ftl_delete', help='Delete FTL bdev')
    p.add_argument('-b', '--name', help="Name of the bdev", required=True)
    p.add_argument('-f', '--fast-shutdown', help="Fast shutdown", action='store_true')
    p.set_defaults(func=bdev_ftl_delete)

    def bdev_ftl_unmap(args):
        print_dict(rpc.bdev.bdev_ftl_unmap(args.client, name=args.name,
                                           lba=args.lba,
                                           num_blocks=args.num_blocks))

    p = subparsers.add_parser('bdev_ftl_unmap', help='FTL unmap')
    p.add_argument('-b', '--name', help="Name of the bdev", required=True)
    p.add_argument('--lba', help='start LBA', required=True, type=int)
    p.add_argument('--num-blocks', help='num blocks', required=True, type=int)
    p.set_defaults(func=bdev_ftl_unmap)

    def bdev_ftl_get_stats(args):
        print_dict(rpc.bdev.bdev_ftl_get_stats(args.client, name=args.name))

    p = subparsers.add_parser('bdev_ftl_get_stats', help='print ftl stats')
    p.add_argument('-b', '--name', help="Name of the bdev", required=True)
    p.set_defaults(func=bdev_ftl_get_stats)

    # vmd
    def vmd_enable(args):
        print_dict(rpc.vmd.vmd_enable(args.client))

    p = subparsers.add_parser('vmd_enable', aliases=['enable_vmd'], help='Enable VMD enumeration')
    p.set_defaults(func=vmd_enable)

    def vmd_remove_device(args):
        print_dict(rpc.vmd.vmd_remove_device(args.client, addr=args.addr))

    p = subparsers.add_parser('vmd_remove_device', help='Remove a device behind VMD')
    p.add_argument('addr', help='Address of the device to remove', type=str)
    p.set_defaults(func=vmd_remove_device)

    def vmd_rescan(args):
        print_dict(rpc.vmd.vmd_rescan(args.client))

    p = subparsers.add_parser('vmd_rescan', help='Force a rescan of the devices behind VMD')
    p.set_defaults(func=vmd_rescan)

    # ublk
    def ublk_create_target(args):
        rpc.ublk.ublk_create_target(args.client,
                                    cpumask=args.cpumask)
    p = subparsers.add_parser('ublk_create_target',
                              help='Create spdk ublk target for ublk dev')
    p.add_argument('-m', '--cpumask', help='cpu mask for ublk dev')
    p.set_defaults(func=ublk_create_target)

    def ublk_destroy_target(args):
        rpc.ublk.ublk_destroy_target(args.client)
    p = subparsers.add_parser('ublk_destroy_target',
                              help='Destroy spdk ublk target for ublk dev')
    p.set_defaults(func=ublk_destroy_target)

    def ublk_start_disk(args):
        print(rpc.ublk.ublk_start_disk(args.client,
                                       bdev_name=args.bdev_name,
                                       ublk_id=args.ublk_id,
                                       num_queues=args.num_queues,
                                       queue_depth=args.queue_depth))

    p = subparsers.add_parser('ublk_start_disk',
                              help='Export a bdev as a ublk device')
    p.add_argument('bdev_name', help='Blockdev name to be exported. Example: Malloc0.')
    p.add_argument('ublk_id', help='ublk device id to be assigned. Example: 1.', type=int)
    p.add_argument('-q', '--num-queues', help="the total number of queues. Example: 1", type=int, required=False)
    p.add_argument('-d', '--queue-depth', help="queue depth. Example: 128", type=int, required=False)
    p.set_defaults(func=ublk_start_disk)

    def ublk_stop_disk(args):
        rpc.ublk.ublk_stop_disk(args.client,
                                ublk_id=args.ublk_id)

    p = subparsers.add_parser('ublk_stop_disk',
                              help='Stop a ublk device')
    p.add_argument('ublk_id', help='ublk device id to be deleted. Example: 1.', type=int)
    p.set_defaults(func=ublk_stop_disk)

    def ublk_get_disks(args):
        print_dict(rpc.ublk.ublk_get_disks(args.client,
                                           ublk_id=args.ublk_id))

    p = subparsers.add_parser('ublk_get_disks',
                              help='Display full or specified ublk device list')
    p.add_argument('-n', '--ublk-id', help="ublk device id. Example: 1", type=int, required=False)
    p.set_defaults(func=ublk_get_disks)

    # nbd
    def nbd_start_disk(args):
        print(rpc.nbd.nbd_start_disk(args.client,
                                     bdev_name=args.bdev_name,
                                     nbd_device=args.nbd_device))

    p = subparsers.add_parser('nbd_start_disk',
                              help='Export a bdev as an nbd disk')
    p.add_argument('bdev_name', help='Blockdev name to be exported. Example: Malloc0.')
    p.add_argument('nbd_device', help='Nbd device name to be assigned. Example: /dev/nbd0.', nargs='?')
    p.set_defaults(func=nbd_start_disk)

    def nbd_stop_disk(args):
        rpc.nbd.nbd_stop_disk(args.client,
                              nbd_device=args.nbd_device)

    p = subparsers.add_parser('nbd_stop_disk',
                              help='Stop an nbd disk')
    p.add_argument('nbd_device', help='Nbd device name to be stopped. Example: /dev/nbd0.')
    p.set_defaults(func=nbd_stop_disk)

    def nbd_get_disks(args):
        print_dict(rpc.nbd.nbd_get_disks(args.client,
                                         nbd_device=args.nbd_device))

    p = subparsers.add_parser('nbd_get_disks',
                              help='Display full or specified nbd device list')
    p.add_argument('-n', '--nbd-device', help="Path of the nbd device. Example: /dev/nbd0", required=False)
    p.set_defaults(func=nbd_get_disks)

    # NVMe-oF
    def nvmf_set_max_subsystems(args):
        rpc.nvmf.nvmf_set_max_subsystems(args.client,
                                         max_subsystems=args.max_subsystems)

    p = subparsers.add_parser('nvmf_set_max_subsystems',
                              help='Set the maximum number of NVMf target subsystems')
    p.add_argument('-x', '--max-subsystems', help='Max number of NVMf subsystems', type=int, required=True)
    p.set_defaults(func=nvmf_set_max_subsystems)

    def nvmf_set_config(args):
        rpc.nvmf.nvmf_set_config(args.client,
                                 passthru_identify_ctrlr=args.passthru_identify_ctrlr,
                                 poll_groups_mask=args.poll_groups_mask,
                                 discovery_filter=args.discovery_filter)

    p = subparsers.add_parser('nvmf_set_config', help='Set NVMf target config')
    p.add_argument('-i', '--passthru-identify-ctrlr', help="""Passthrough fields like serial number and model number
    when the controller has a single namespace that is an NVMe bdev""", action='store_true')
    p.add_argument('-m', '--poll-groups-mask', help='Set cpumask for NVMf poll groups (optional)', type=str)
    p.add_argument('-d', '--discovery-filter', help="""Set discovery filter (optional), possible values are: `match_any` (default) or
         comma separated values: `transport`, `address`, `svcid`""", type=str)
    p.set_defaults(func=nvmf_set_config)

    def nvmf_create_transport(args):
        rpc.nvmf.nvmf_create_transport(**vars(args))

    p = subparsers.add_parser('nvmf_create_transport', help='Create NVMf transport')
    p.add_argument('-t', '--trtype', help='Transport type (ex. RDMA)', type=str, required=True)
    p.add_argument('-g', '--tgt-name', help='The name of the parent NVMe-oF target (optional)', type=str)
    p.add_argument('-q', '--max-queue-depth', help='Max number of outstanding I/O per queue', type=int)
    p.add_argument('-m', '--max-io-qpairs-per-ctrlr', help='Max number of IO qpairs per controller', type=int)
    p.add_argument('-c', '--in-capsule-data-size', help='Max number of in-capsule data size', type=int)
    p.add_argument('-i', '--max-io-size', help='Max I/O size (bytes)', type=int)
    p.add_argument('-u', '--io-unit-size', help='I/O unit size (bytes)', type=int)
    p.add_argument('-a', '--max-aq-depth', help='Max number of admin cmds per AQ', type=int)
    p.add_argument('-n', '--num-shared-buffers', help='The number of pooled data buffers available to the transport', type=int)
    p.add_argument('-b', '--buf-cache-size', help='The number of shared buffers to reserve for each poll group', type=int)
    p.add_argument('-z', '--zcopy', action='store_true', help='''Use zero-copy operations if the
    underlying bdev supports them''')
    p.add_argument('-d', '--num-cqe', help="""The number of CQ entries. Only used when no_srq=true.
    Relevant only for RDMA transport""", type=int)
    p.add_argument('-s', '--max-srq-depth', help='Max number of outstanding I/O per SRQ. Relevant only for RDMA transport', type=int)
    p.add_argument('-r', '--no-srq', action='store_true', help='Disable per-thread shared receive queue. Relevant only for RDMA transport')
    p.add_argument('-o', '--c2h-success', action='store_false', help='Disable C2H success optimization. Relevant only for TCP transport')
    p.add_argument('-f', '--dif-insert-or-strip', action='store_true', help='Enable DIF insert/strip. Relevant only for TCP transport')
    p.add_argument('-y', '--sock-priority', help='The sock priority of the tcp connection. Relevant only for TCP transport', type=int)
    p.add_argument('-l', '--acceptor-backlog', help='Pending connections allowed at one time. Relevant only for RDMA transport', type=int)
    p.add_argument('-x', '--abort-timeout-sec', help='Abort execution timeout value, in seconds', type=int)
    p.add_argument('-w', '--no-wr-batching', action='store_true', help='Disable work requests batching. Relevant only for RDMA transport')
    p.add_argument('-e', '--control-msg-num', help="""The number of control messages per poll group.
    Relevant only for TCP transport""", type=int)
    p.add_argument('-M', '--disable-mappable-bar0', action='store_true', help="""Disable mmap() of BAR0.
    Relevant only for VFIO-USER transport""")
    p.add_argument('-I', '--disable-adaptive-irq', action='store_true', help="""Disable adaptive interrupt feature.
    Relevant only for VFIO-USER transport""")
    p.add_argument('-S', '--disable-shadow-doorbells', action='store_true', help="""Disable shadow doorbell support.
    Relevant only for VFIO-USER transport""")
    p.add_argument('--acceptor-poll-rate', help='Polling interval of the acceptor for incoming connections (usec)', type=int)
    p.set_defaults(func=nvmf_create_transport)

    def nvmf_get_transports(args):
        print_dict(rpc.nvmf.nvmf_get_transports(args.client, trtype=args.trtype, tgt_name=args.tgt_name))

    p = subparsers.add_parser('nvmf_get_transports', help='Display nvmf transports or required transport')
    p.add_argument('--trtype', help='Transport type (optional)')
    p.add_argument('-t', '--tgt-name', help='The name of the parent NVMe-oF target (optional)', type=str)
    p.set_defaults(func=nvmf_get_transports)

    def nvmf_get_subsystems(args):
        print_dict(rpc.nvmf.nvmf_get_subsystems(args.client, nqn=args.nqn, tgt_name=args.tgt_name))

    p = subparsers.add_parser('nvmf_get_subsystems', help='Display nvmf subsystems or required subsystem')
    p.add_argument('nqn', help='Subsystem NQN (optional)', nargs="?")
    p.add_argument('-t', '--tgt-name', help='The name of the parent NVMe-oF target (optional)', type=str)
    p.set_defaults(func=nvmf_get_subsystems)

    def nvmf_create_subsystem(args):
        rpc.nvmf.nvmf_create_subsystem(args.client,
                                       nqn=args.nqn,
                                       tgt_name=args.tgt_name,
                                       serial_number=args.serial_number,
                                       model_number=args.model_number,
                                       allow_any_host=args.allow_any_host,
                                       max_namespaces=args.max_namespaces,
                                       ana_reporting=args.ana_reporting,
                                       min_cntlid=args.min_cntlid,
                                       max_cntlid=args.max_cntlid)

    p = subparsers.add_parser('nvmf_create_subsystem', help='Create an NVMe-oF subsystem')
    p.add_argument('nqn', help='Subsystem NQN (ASCII)')
    p.add_argument('-t', '--tgt-name', help='The name of the parent NVMe-oF target (optional)', type=str)
    p.add_argument("-s", "--serial-number", help="""
    Format:  'sn' etc
    Example: 'SPDK00000000000001'""")
    p.add_argument("-d", "--model-number", help="""
    Format:  'mn' etc
    Example: 'SPDK Controller'""")
    p.add_argument("-a", "--allow-any-host", action='store_true', help="Allow any host to connect (don't enforce allowed host NQN list)")
    p.add_argument("-m", "--max-namespaces", help="Maximum number of namespaces allowed",
                   type=int)
    p.add_argument("-r", "--ana-reporting", action='store_true', help="Enable ANA reporting feature")
    p.add_argument("-i", "--min_cntlid", help="Minimum controller ID", type=int)
    p.add_argument("-I", "--max_cntlid", help="Maximum controller ID", type=int)
    p.set_defaults(func=nvmf_create_subsystem)

    def nvmf_delete_subsystem(args):
        rpc.nvmf.nvmf_delete_subsystem(args.client,
                                       nqn=args.subsystem_nqn,
                                       tgt_name=args.tgt_name)

    p = subparsers.add_parser('nvmf_delete_subsystem', help='Delete a nvmf subsystem')
    p.add_argument('subsystem_nqn',
                   help='subsystem nqn to be deleted. Example: nqn.2016-06.io.spdk:cnode1.')
    p.add_argument('-t', '--tgt-name', help='The name of the parent NVMe-oF target (optional)', type=str)
    p.set_defaults(func=nvmf_delete_subsystem)

    def nvmf_subsystem_add_listener(args):
        rpc.nvmf.nvmf_subsystem_add_listener(**vars(args))

    p = subparsers.add_parser('nvmf_subsystem_add_listener', help='Add a listener to an NVMe-oF subsystem')
    p.add_argument('nqn', help='NVMe-oF subsystem NQN (\'discovery\' can be used as shortcut for discovery NQN)')
    p.add_argument('-t', '--trtype', help='NVMe-oF transport type: e.g., rdma', required=True)
    p.add_argument('-a', '--traddr', help='NVMe-oF transport address: e.g., an ip address', required=True)
    p.add_argument('-p', '--tgt-name', help='The name of the parent NVMe-oF target (optional)', type=str)
    p.add_argument('-f', '--adrfam', help='NVMe-oF transport adrfam: e.g., ipv4, ipv6, ib, fc, intra_host')
    p.add_argument('-s', '--trsvcid', help='NVMe-oF transport service id: e.g., a port number (required for RDMA or TCP)')
    p.add_argument('-k', '--secure-channel', help='Immediately establish a secure channel', action="store_true")
    p.set_defaults(func=nvmf_subsystem_add_listener)

    def nvmf_subsystem_remove_listener(args):
        rpc.nvmf.nvmf_subsystem_remove_listener(args.client,
                                                nqn=args.nqn,
                                                trtype=args.trtype,
                                                traddr=args.traddr,
                                                tgt_name=args.tgt_name,
                                                adrfam=args.adrfam,
                                                trsvcid=args.trsvcid)

    p = subparsers.add_parser('nvmf_subsystem_remove_listener', help='Remove a listener from an NVMe-oF subsystem')
    p.add_argument('nqn', help='NVMe-oF subsystem NQN (\'discovery\' can be used as shortcut for discovery NQN)')
    p.add_argument('-t', '--trtype', help='NVMe-oF transport type: e.g., rdma', required=True)
    p.add_argument('-a', '--traddr', help='NVMe-oF transport address: e.g., an ip address', required=True)
    p.add_argument('-p', '--tgt-name', help='The name of the parent NVMe-oF target (optional)', type=str)
    p.add_argument('-f', '--adrfam', help='NVMe-oF transport adrfam: e.g., ipv4, ipv6, ib, fc, intra_host')
    p.add_argument('-s', '--trsvcid', help='NVMe-oF transport service id: e.g., a port number (required for TCP and RDMA transport types)')
    p.set_defaults(func=nvmf_subsystem_remove_listener)

    def nvmf_subsystem_listener_set_ana_state(args):
        rpc.nvmf.nvmf_subsystem_listener_set_ana_state(args.client,
                                                       nqn=args.nqn,
                                                       ana_state=args.ana_state,
                                                       trtype=args.trtype,
                                                       traddr=args.traddr,
                                                       tgt_name=args.tgt_name,
                                                       adrfam=args.adrfam,
                                                       trsvcid=args.trsvcid,
                                                       anagrpid=args.anagrpid)

    p = subparsers.add_parser('nvmf_subsystem_listener_set_ana_state', help='Set ANA state of a listener for an NVMe-oF subsystem')
    p.add_argument('nqn', help='NVMe-oF subsystem NQN')
    p.add_argument('-n', '--ana-state', help='ANA state to set: optimized, non_optimized, or inaccessible', required=True)
    p.add_argument('-t', '--trtype', help='NVMe-oF transport type: e.g., rdma', required=True)
    p.add_argument('-a', '--traddr', help='NVMe-oF transport address: e.g., an ip address', required=True)
    p.add_argument('-p', '--tgt-name', help='The name of the parent NVMe-oF target (optional)', type=str)
    p.add_argument('-f', '--adrfam', help='NVMe-oF transport adrfam: e.g., ipv4, ipv6, ib, fc, intra_host')
    p.add_argument('-s', '--trsvcid', help='NVMe-oF transport service id: e.g., a port number')
    p.add_argument('-g', '--anagrpid', help='ANA group ID (optional)', type=int)
    p.set_defaults(func=nvmf_subsystem_listener_set_ana_state)

    def nvmf_subsystem_add_ns(args):
        rpc.nvmf.nvmf_subsystem_add_ns(args.client,
                                       nqn=args.nqn,
                                       bdev_name=args.bdev_name,
                                       tgt_name=args.tgt_name,
                                       ptpl_file=args.ptpl_file,
                                       nsid=args.nsid,
                                       nguid=args.nguid,
                                       eui64=args.eui64,
                                       uuid=args.uuid,
                                       anagrpid=args.anagrpid)

    p = subparsers.add_parser('nvmf_subsystem_add_ns', help='Add a namespace to an NVMe-oF subsystem')
    p.add_argument('nqn', help='NVMe-oF subsystem NQN')
    p.add_argument('bdev_name', help='The name of the bdev that will back this namespace')
    p.add_argument('-t', '--tgt-name', help='The name of the parent NVMe-oF target (optional)', type=str)
    p.add_argument('-p', '--ptpl-file', help='The persistent reservation storage location (optional)', type=str)
    p.add_argument('-n', '--nsid', help='The requested NSID (optional)', type=int)
    p.add_argument('-g', '--nguid', help='Namespace globally unique identifier (optional)')
    p.add_argument('-e', '--eui64', help='Namespace EUI-64 identifier (optional)')
    p.add_argument('-u', '--uuid', help='Namespace UUID (optional)')
    p.add_argument('-a', '--anagrpid', help='ANA group ID (optional)', type=int)
    p.set_defaults(func=nvmf_subsystem_add_ns)

    def nvmf_subsystem_remove_ns(args):
        rpc.nvmf.nvmf_subsystem_remove_ns(args.client,
                                          nqn=args.nqn,
                                          nsid=args.nsid,
                                          tgt_name=args.tgt_name)

    p = subparsers.add_parser('nvmf_subsystem_remove_ns', help='Remove a namespace to an NVMe-oF subsystem')
    p.add_argument('nqn', help='NVMe-oF subsystem NQN')
    p.add_argument('nsid', help='The requested NSID', type=int)
    p.add_argument('-t', '--tgt-name', help='The name of the parent NVMe-oF target (optional)', type=str)
    p.set_defaults(func=nvmf_subsystem_remove_ns)

    def nvmf_subsystem_add_host(args):
        rpc.nvmf.nvmf_subsystem_add_host(args.client,
                                         nqn=args.nqn,
                                         host=args.host,
                                         tgt_name=args.tgt_name,
                                         psk=args.psk)

    p = subparsers.add_parser('nvmf_subsystem_add_host', help='Add a host to an NVMe-oF subsystem')
    p.add_argument('nqn', help='NVMe-oF subsystem NQN')
    p.add_argument('host', help='Host NQN to allow')
    p.add_argument('-t', '--tgt-name', help='The name of the parent NVMe-oF target (optional)', type=str)
    p.add_argument('--psk', help='Path to PSK file for TLS authentication (optional). Only applicable for TCP transport.', type=str)
    p.set_defaults(func=nvmf_subsystem_add_host)

    def nvmf_subsystem_remove_host(args):
        rpc.nvmf.nvmf_subsystem_remove_host(args.client,
                                            nqn=args.nqn,
                                            host=args.host,
                                            tgt_name=args.tgt_name)

    p = subparsers.add_parser('nvmf_subsystem_remove_host', help='Remove a host from an NVMe-oF subsystem')
    p.add_argument('nqn', help='NVMe-oF subsystem NQN')
    p.add_argument('host', help='Host NQN to remove')
    p.add_argument('-t', '--tgt-name', help='The name of the parent NVMe-oF target (optional)', type=str)
    p.set_defaults(func=nvmf_subsystem_remove_host)

    def nvmf_subsystem_allow_any_host(args):
        rpc.nvmf.nvmf_subsystem_allow_any_host(args.client,
                                               nqn=args.nqn,
                                               disable=args.disable,
                                               tgt_name=args.tgt_name)

    p = subparsers.add_parser('nvmf_subsystem_allow_any_host', help='Allow any host to connect to the subsystem')
    p.add_argument('nqn', help='NVMe-oF subsystem NQN')
    p.add_argument('-e', '--enable', action='store_true', help='Enable allowing any host')
    p.add_argument('-d', '--disable', action='store_true', help='Disable allowing any host')
    p.add_argument('-t', '--tgt-name', help='The name of the parent NVMe-oF target (optional)', type=str)
    p.set_defaults(func=nvmf_subsystem_allow_any_host)

    def nvmf_subsystem_get_controllers(args):
        print_dict(rpc.nvmf.nvmf_subsystem_get_controllers(args.client,
                                                           nqn=args.nqn,
                                                           tgt_name=args.tgt_name))

    p = subparsers.add_parser('nvmf_subsystem_get_controllers',
                              help='Display controllers of an NVMe-oF subsystem.')
    p.add_argument('nqn', help='NVMe-oF subsystem NQN')
    p.add_argument('-t', '--tgt-name', help='The name of the parent NVMe-oF target (optional)', type=str)
    p.set_defaults(func=nvmf_subsystem_get_controllers)

    def nvmf_subsystem_get_qpairs(args):
        print_dict(rpc.nvmf.nvmf_subsystem_get_qpairs(args.client,
                                                      nqn=args.nqn,
                                                      tgt_name=args.tgt_name))

    p = subparsers.add_parser('nvmf_subsystem_get_qpairs',
                              help='Display queue pairs of an NVMe-oF subsystem.')
    p.add_argument('nqn', help='NVMe-oF subsystem NQN')
    p.add_argument('-t', '--tgt-name', help='The name of the parent NVMe-oF target (optional)', type=str)
    p.set_defaults(func=nvmf_subsystem_get_qpairs)

    def nvmf_subsystem_get_listeners(args):
        print_dict(rpc.nvmf.nvmf_subsystem_get_listeners(args.client,
                                                         nqn=args.nqn,
                                                         tgt_name=args.tgt_name))

    p = subparsers.add_parser('nvmf_subsystem_get_listeners',
                              help='Display listeners of an NVMe-oF subsystem.')
    p.add_argument('nqn', help='NVMe-oF subsystem NQN')
    p.add_argument('-t', '--tgt-name', help='The name of the parent NVMe-oF target (optional)', type=str)
    p.set_defaults(func=nvmf_subsystem_get_listeners)

    def nvmf_get_stats(args):
        print_dict(rpc.nvmf.nvmf_get_stats(args.client, tgt_name=args.tgt_name))

    p = subparsers.add_parser(
        'nvmf_get_stats', help='Display current statistics for NVMf subsystem')
    p.add_argument('-t', '--tgt-name', help='The name of the parent NVMe-oF target (optional)', type=str)
    p.set_defaults(func=nvmf_get_stats)

    def nvmf_set_crdt(args):
        print_dict(rpc.nvmf.nvmf_set_crdt(args.client, args.crdt1, args.crdt2, args.crdt3))

    p = subparsers.add_parser(
        'nvmf_set_crdt',
        help="""Set the 3 crdt (Command Retry Delay Time) values for NVMf subsystem. All
        values are in units of 100 milliseconds (same as the NVM Express specification).""")
    p.add_argument('-t1', '--crdt1', help='Command Retry Delay Time 1, in units of 100 milliseconds', type=int)
    p.add_argument('-t2', '--crdt2', help='Command Retry Delay Time 2, in units of 100 milliseconds', type=int)
    p.add_argument('-t3', '--crdt3', help='Command Retry Delay Time 3, in units of 100 milliseconds', type=int)
    p.set_defaults(func=nvmf_set_crdt)

    # subsystem
    def framework_get_subsystems(args):
        print_dict(rpc.subsystem.framework_get_subsystems(args.client))

    p = subparsers.add_parser('framework_get_subsystems',
                              help="""Print subsystems array in initialization order. Each subsystem
    entry contain (unsorted) array of subsystems it depends on.""")
    p.set_defaults(func=framework_get_subsystems)

    def framework_get_config(args):
        print_dict(rpc.subsystem.framework_get_config(args.client, args.name))

    p = subparsers.add_parser('framework_get_config', help="""Print subsystem configuration""")
    p.add_argument('name', help='Name of subsystem to query')
    p.set_defaults(func=framework_get_config)

    # vhost
    def vhost_controller_set_coalescing(args):
        rpc.vhost.vhost_controller_set_coalescing(args.client,
                                                  ctrlr=args.ctrlr,
                                                  delay_base_us=args.delay_base_us,
                                                  iops_threshold=args.iops_threshold)

    p = subparsers.add_parser('vhost_controller_set_coalescing', help='Set vhost controller coalescing')
    p.add_argument('ctrlr', help='controller name')
    p.add_argument('delay_base_us', help='Base delay time', type=int)
    p.add_argument('iops_threshold', help='IOPS threshold when coalescing is enabled', type=int)
    p.set_defaults(func=vhost_controller_set_coalescing)

    def virtio_blk_create_transport(args):
        rpc.vhost.virtio_blk_create_transport(**vars(args))

    p = subparsers.add_parser('virtio_blk_create_transport',
                              help='Create virtio blk transport')
    p.add_argument('name', help='transport name')
    p.set_defaults(func=virtio_blk_create_transport)

    def virtio_blk_get_transports(args):
        print_dict(rpc.vhost.virtio_blk_get_transports(args.client, name=args.name))

    p = subparsers.add_parser('virtio_blk_get_transports', help='Display virtio-blk transports or requested transport')
    p.add_argument('--name', help='Transport name (optional)', type=str)
    p.set_defaults(func=virtio_blk_get_transports)

    def vhost_create_scsi_controller(args):
        rpc.vhost.vhost_create_scsi_controller(args.client,
                                               ctrlr=args.ctrlr,
                                               cpumask=args.cpumask)

    p = subparsers.add_parser('vhost_create_scsi_controller', help='Add new vhost controller')
    p.add_argument('ctrlr', help='controller name')
    p.add_argument('--cpumask', help='cpu mask for this controller')
    p.set_defaults(func=vhost_create_scsi_controller)

    def vhost_scsi_controller_add_target(args):
        print_json(rpc.vhost.vhost_scsi_controller_add_target(args.client,
                                                              ctrlr=args.ctrlr,
                                                              scsi_target_num=args.scsi_target_num,
                                                              bdev_name=args.bdev_name))

    p = subparsers.add_parser('vhost_scsi_controller_add_target', help='Add lun to vhost controller')
    p.add_argument('ctrlr', help='controller name where add lun')
    p.add_argument('scsi_target_num', help='scsi_target_num', type=int)
    p.add_argument('bdev_name', help='bdev name')
    p.set_defaults(func=vhost_scsi_controller_add_target)

    def vhost_scsi_controller_remove_target(args):
        rpc.vhost.vhost_scsi_controller_remove_target(args.client,
                                                      ctrlr=args.ctrlr,
                                                      scsi_target_num=args.scsi_target_num)

    p = subparsers.add_parser('vhost_scsi_controller_remove_target',
                              help='Remove target from vhost controller')
    p.add_argument('ctrlr', help='controller name to remove target from')
    p.add_argument('scsi_target_num', help='scsi_target_num', type=int)
    p.set_defaults(func=vhost_scsi_controller_remove_target)

    def vhost_create_blk_controller(args):
        rpc.vhost.vhost_create_blk_controller(**vars(args))

    p = subparsers.add_parser('vhost_create_blk_controller', help='Add a new vhost block controller')
    p.add_argument('ctrlr', help='controller name')
    p.add_argument('dev_name', help='device name')
    p.add_argument('--cpumask', help='cpu mask for this controller')
    p.add_argument('--transport', help='virtio blk transport name (default: vhost_user_blk)')
    p.add_argument("-r", "--readonly", action='store_true', help='Set controller as read-only')
    p.add_argument("-p", "--packed_ring", action='store_true', help='Set controller as packed ring supported')
    p.add_argument("-l", "--packed_ring_recovery", action='store_true', help='Enable packed ring live recovery')
    p.set_defaults(func=vhost_create_blk_controller)

    def vhost_get_controllers(args):
        print_dict(rpc.vhost.vhost_get_controllers(args.client, args.name))

    p = subparsers.add_parser('vhost_get_controllers', help='List all or specific vhost controller(s)')
    p.add_argument('-n', '--name', help="Name of vhost controller", required=False)
    p.set_defaults(func=vhost_get_controllers)

    def vhost_delete_controller(args):
        rpc.vhost.vhost_delete_controller(args.client,
                                          ctrlr=args.ctrlr)

    p = subparsers.add_parser('vhost_delete_controller', help='Delete a vhost controller')
    p.add_argument('ctrlr', help='controller name')
    p.set_defaults(func=vhost_delete_controller)

    def bdev_virtio_attach_controller(args):
        print_array(rpc.vhost.bdev_virtio_attach_controller(args.client,
                                                            name=args.name,
                                                            trtype=args.trtype,
                                                            traddr=args.traddr,
                                                            dev_type=args.dev_type,
                                                            vq_count=args.vq_count,
                                                            vq_size=args.vq_size))

    p = subparsers.add_parser('bdev_virtio_attach_controller',
                              help="""Attach virtio controller using provided
    transport type and device type. This will also create bdevs for any block devices connected to the
    controller (for example, SCSI devices for a virtio-scsi controller).
    Result is array of added bdevs.""")
    p.add_argument('name', help="Use this name as base for new created bdevs")
    p.add_argument('-t', '--trtype',
                   help='Virtio target transport type: pci or user', required=True)
    p.add_argument('-a', '--traddr',
                   help='Transport type specific target address: e.g. UNIX domain socket path or BDF', required=True)
    p.add_argument('-d', '--dev-type',
                   help='Device type: blk or scsi', required=True)
    p.add_argument('--vq-count', help='Number of virtual queues to be used.', type=int)
    p.add_argument('--vq-size', help='Size of each queue', type=int)
    p.set_defaults(func=bdev_virtio_attach_controller)

    def bdev_virtio_scsi_get_devices(args):
        print_dict(rpc.vhost.bdev_virtio_scsi_get_devices(args.client))

    p = subparsers.add_parser('bdev_virtio_scsi_get_devices', help='List all Virtio-SCSI devices.')
    p.set_defaults(func=bdev_virtio_scsi_get_devices)

    def bdev_virtio_detach_controller(args):
        rpc.vhost.bdev_virtio_detach_controller(args.client,
                                                name=args.name)

    p = subparsers.add_parser('bdev_virtio_detach_controller', help="""Remove a Virtio device
    This will delete all bdevs exposed by this device""")
    p.add_argument('name', help='Virtio device name. E.g. VirtioUser0')
    p.set_defaults(func=bdev_virtio_detach_controller)

    def bdev_virtio_blk_set_hotplug(args):
        rpc.vhost.bdev_virtio_blk_set_hotplug(args.client, enable=args.enable, period_us=args.period_us)

    p = subparsers.add_parser('bdev_virtio_blk_set_hotplug', help='Set hotplug options for bdev virtio_blk type.')
    p.add_argument('-d', '--disable', dest='enable', default=False, action='store_false', help="Disable hotplug (default)")
    p.add_argument('-e', '--enable', dest='enable', action='store_true', help="Enable hotplug")
    p.add_argument('-r', '--period-us',
                   help='How often the hotplug is processed for insert and remove events', type=int)
    p.set_defaults(func=bdev_virtio_blk_set_hotplug)

    # vfio-user target
    def vfu_tgt_set_base_path(args):
        rpc.vfio_user.vfu_tgt_set_base_path(args.client, path=args.path)

    p = subparsers.add_parser('vfu_tgt_set_base_path', help='Set socket base path.')
    p.add_argument('path', help='socket base path')
    p.set_defaults(func=vfu_tgt_set_base_path)

    def vfu_virtio_delete_endpoint(args):
        rpc.vfio_user.vfu_virtio_delete_endpoint(args.client, name=args.name)

    p = subparsers.add_parser('vfu_virtio_delete_endpoint', help='Delete the PCI device via endpoint name.')
    p.add_argument('name', help='Endpoint name')
    p.set_defaults(func=vfu_virtio_delete_endpoint)

    def vfu_virtio_create_blk_endpoint(args):
        rpc.vfio_user.vfu_virtio_create_blk_endpoint(args.client,
                                                     name=args.name,
                                                     bdev_name=args.bdev_name,
                                                     cpumask=args.cpumask,
                                                     num_queues=args.num_queues,
                                                     qsize=args.qsize,
                                                     packed_ring=args.packed_ring)

    p = subparsers.add_parser('vfu_virtio_create_blk_endpoint', help='Create virtio-blk endpoint.')
    p.add_argument('name', help='Name of the endpoint')
    p.add_argument('--bdev-name', help='block device name', type=str, required=True)
    p.add_argument('--cpumask', help='CPU masks')
    p.add_argument('--num-queues', help='number of vrings', type=int, default=0)
    p.add_argument('--qsize', help='number of element for each vring', type=int, default=0)
    p.add_argument("--packed-ring", action='store_true', help='Enable packed ring')
    p.set_defaults(func=vfu_virtio_create_blk_endpoint)

    def vfu_virtio_scsi_add_target(args):
        rpc.vfio_user.vfu_virtio_scsi_add_target(args.client,
                                                 name=args.name,
                                                 scsi_target_num=args.scsi_target_num,
                                                 bdev_name=args.bdev_name)

    p = subparsers.add_parser('vfu_virtio_scsi_add_target', help='Attach a block device to SCSI target of PCI endpoint.')
    p.add_argument('name', help='Name of the endpoint')
    p.add_argument('--scsi-target-num', help='number of SCSI Target', type=int, required=True)
    p.add_argument('--bdev-name', help='block device name', type=str, required=True)
    p.set_defaults(func=vfu_virtio_scsi_add_target)

    def vfu_virtio_scsi_remove_target(args):
        rpc.vfio_user.vfu_virtio_scsi_remove_target(args.client,
                                                    name=args.name,
                                                    scsi_target_num=args.scsi_target_num)

    p = subparsers.add_parser('vfu_virtio_scsi_remove_target', help='Remove the specified SCSI target of PCI endpoint.')
    p.add_argument('name', help='Name of the endpoint')
    p.add_argument('--scsi-target-num', help='number of SCSI Target', type=int, required=True)
    p.set_defaults(func=vfu_virtio_scsi_remove_target)

    def vfu_virtio_create_scsi_endpoint(args):
        rpc.vfio_user.vfu_virtio_create_scsi_endpoint(args.client,
                                                      name=args.name,
                                                      cpumask=args.cpumask,
                                                      num_io_queues=args.num_io_queues,
                                                      qsize=args.qsize,
                                                      packed_ring=args.packed_ring)

    p = subparsers.add_parser('vfu_virtio_create_scsi_endpoint', help='Create virtio-scsi endpoint.')
    p.add_argument('name', help='Name of the endpoint')
    p.add_argument('--cpumask', help='CPU masks')
    p.add_argument('--num-io-queues', help='number of IO vrings', type=int, default=0)
    p.add_argument('--qsize', help='number of element for each vring', type=int, default=0)
    p.add_argument("--packed-ring", action='store_true', help='Enable packed ring')
    p.set_defaults(func=vfu_virtio_create_scsi_endpoint)

    # accel_fw
    def accel_get_opc_assignments(args):
        print_dict(rpc.accel.accel_get_opc_assignments(args.client))

    p = subparsers.add_parser('accel_get_opc_assignments', help='Get list of opcode name to module assignments.')
    p.set_defaults(func=accel_get_opc_assignments)

    def accel_get_module_info(args):
        print_dict(rpc.accel.accel_get_module_info(args.client))

    p = subparsers.add_parser('accel_get_module_info', aliases=['accel_get_engine_info'],
                              help='Get list of valid module names and their operations.')
    p.set_defaults(func=accel_get_module_info)

    def accel_assign_opc(args):
        rpc.accel.accel_assign_opc(args.client, opname=args.opname, module=args.module)

    p = subparsers.add_parser('accel_assign_opc', help='Manually assign an operation to a module.')
    p.add_argument('-o', '--opname', help='opname')
    p.add_argument('-m', '--module', help='name of module')
    p.set_defaults(func=accel_assign_opc)

    def accel_crypto_key_create(args):
        print_dict(rpc.accel.accel_crypto_key_create(args.client,
                                                     cipher=args.cipher,
                                                     key=args.key,
                                                     key2=args.key2,
                                                     tweak_mode=args.tweak_mode,
                                                     name=args.name))

    p = subparsers.add_parser('accel_crypto_key_create', help='Create encryption key')
    p.add_argument('-c', '--cipher', help='cipher', required=True)
    p.add_argument('-k', '--key', help='key', required=True)
    p.add_argument('-e', '--key2', help='key2', required=False, default=None)
    p.add_argument('-t', '--tweak-mode', help='tweak mode', required=False, default=None)
    p.add_argument('-n', '--name', help='key name', required=True)
    p.set_defaults(func=accel_crypto_key_create)

    def accel_crypto_key_destroy(args):
        print_dict(rpc.accel.accel_crypto_key_destroy(args.client,
                                                      key_name=args.name))

    p = subparsers.add_parser('accel_crypto_key_destroy', help='Destroy encryption key')
    p.add_argument('-n', '--name', help='key name', required=True, type=str)
    p.set_defaults(func=accel_crypto_key_destroy)

    def accel_crypto_keys_get(args):
        print_dict(rpc.accel.accel_crypto_keys_get(args.client,
                                                   key_name=args.key_name))

    p = subparsers.add_parser('accel_crypto_keys_get', help='Get a list of the crypto keys')
    p.add_argument('-k', '--key-name', help='Get information about a specific key', type=str)
    p.set_defaults(func=accel_crypto_keys_get)

    def accel_set_driver(args):
        rpc.accel.accel_set_driver(args.client, name=args.name)

    p = subparsers.add_parser('accel_set_driver', help='Select accel platform driver to execute ' +
                              'operation chains')
    p.add_argument('name', help='name of the platform driver')
    p.set_defaults(func=accel_set_driver)

    def accel_set_options(args):
        rpc.accel.accel_set_options(args.client, args.small_cache_size, args.large_cache_size,
                                    args.task_count, args.sequence_count, args.buf_count)

    p = subparsers.add_parser('accel_set_options', help='Set accel framework\'s options')
    p.add_argument('--small-cache-size', type=int, help='Size of the small iobuf cache')
    p.add_argument('--large-cache-size', type=int, help='Size of the large iobuf cache')
    p.add_argument('--task-count', type=int, help='Maximum number of tasks per IO channel')
    p.add_argument('--sequence-count', type=int, help='Maximum number of sequences per IO channel')
    p.add_argument('--buf-count', type=int, help='Maximum number of buffers per IO channel')
    p.set_defaults(func=accel_set_options)

    def accel_get_stats(args):
        print_dict(rpc.accel.accel_get_stats(args.client))

    p = subparsers.add_parser('accel_get_stats', help='Display accel framework\'s statistics')
    p.set_defaults(func=accel_get_stats)

    # ioat
    def ioat_scan_accel_module(args):
        rpc.ioat.ioat_scan_accel_module(args.client)

    p = subparsers.add_parser('ioat_scan_accel_module', aliases=['ioat_scan_accel_engine'],
                              help='Enable IOAT accel module offload.')
    p.set_defaults(func=ioat_scan_accel_module)

    # dpdk compressdev
    def compressdev_scan_accel_module(args):
        rpc.compressdev.compressdev_scan_accel_module(args.client, pmd=args.pmd)

    p = subparsers.add_parser('compressdev_scan_accel_module', help='Scan and enable compressdev module and set pmd option.')
    p.add_argument('-p', '--pmd', type=int, help='0 = auto-select, 1= QAT only, 2 = mlx5_pci only')
    p.set_defaults(func=compressdev_scan_accel_module)

    # dsa
    def dsa_scan_accel_module(args):
        rpc.dsa.dsa_scan_accel_module(args.client, config_kernel_mode=args.config_kernel_mode)

    p = subparsers.add_parser('dsa_scan_accel_module', aliases=['dsa_scan_accel_engine'],
                              help='Set config and enable dsa accel module offload.')
    p.add_argument('-k', '--config-kernel-mode', help='Use Kernel mode dsa',
                   action='store_true', dest='config_kernel_mode')
    p.set_defaults(func=dsa_scan_accel_module, config_kernel_mode=None)

    # iaa
    def iaa_scan_accel_module(args):
        rpc.iaa.iaa_scan_accel_module(args.client)

    p = subparsers.add_parser('iaa_scan_accel_module', aliases=['iaa_scan_accel_engine'],
                              help='Set config and enable iaa accel module offload.')
    p.set_defaults(func=iaa_scan_accel_module)

    def dpdk_cryptodev_scan_accel_module(args):
        rpc.dpdk_cryptodev.dpdk_cryptodev_scan_accel_module(args.client)

    p = subparsers.add_parser('dpdk_cryptodev_scan_accel_module',
                              help='Enable dpdk_cryptodev accel module offload.')
    p.set_defaults(func=dpdk_cryptodev_scan_accel_module)

    def dpdk_cryptodev_set_driver(args):
        rpc.dpdk_cryptodev.dpdk_cryptodev_set_driver(args.client,
                                                     driver_name=args.driver_name)

    p = subparsers.add_parser('dpdk_cryptodev_set_driver',
                              help='Set the DPDK cryptodev driver.')
    p.add_argument('-d', '--driver-name', help='The driver, can be one of crypto_aesni_mb, crypto_qat or mlx5_pci', type=str)
    p.set_defaults(func=dpdk_cryptodev_set_driver)

    def dpdk_cryptodev_get_driver(args):
        print_dict(rpc.dpdk_cryptodev.dpdk_cryptodev_get_driver(args.client))

    p = subparsers.add_parser('dpdk_cryptodev_get_driver', help='Get the DPDK cryptodev driver')
    p.set_defaults(func=dpdk_cryptodev_get_driver)

    # mlx5
    def mlx5_scan_accel_module(args):
        rpc.mlx5.mlx5_scan_accel_module(args.client,
                                        qp_size=args.qp_size,
                                        num_requests=args.num_requests)

    p = subparsers.add_parser('mlx5_scan_accel_module', help='Enable mlx5 accel module.')
    p.add_argument('-q', '--qp-size', type=int, help='QP size')
    p.add_argument('-r', '--num-requests', type=int, help='Size of the shared requests pool')
    p.set_defaults(func=mlx5_scan_accel_module)

    # opal
    def bdev_nvme_opal_init(args):
        rpc.nvme.bdev_nvme_opal_init(args.client,
                                     nvme_ctrlr_name=args.nvme_ctrlr_name,
                                     password=args.password)

    p = subparsers.add_parser('bdev_nvme_opal_init', help='take ownership and activate')
    p.add_argument('-b', '--nvme-ctrlr-name', help='nvme ctrlr name')
    p.add_argument('-p', '--password', help='password for admin')
    p.set_defaults(func=bdev_nvme_opal_init)

    def bdev_nvme_opal_revert(args):
        rpc.nvme.bdev_nvme_opal_revert(args.client,
                                       nvme_ctrlr_name=args.nvme_ctrlr_name,
                                       password=args.password)
    p = subparsers.add_parser('bdev_nvme_opal_revert', help='Revert to default factory settings')
    p.add_argument('-b', '--nvme-ctrlr-name', help='nvme ctrlr name')
    p.add_argument('-p', '--password', help='password')
    p.set_defaults(func=bdev_nvme_opal_revert)

    def bdev_opal_create(args):
        print_json(rpc.bdev.bdev_opal_create(args.client,
                                             nvme_ctrlr_name=args.nvme_ctrlr_name,
                                             nsid=args.nsid,
                                             locking_range_id=args.locking_range_id,
                                             range_start=args.range_start,
                                             range_length=args.range_length,
                                             password=args.password))

    p = subparsers.add_parser('bdev_opal_create', help="""Create opal bdev on specified NVMe controller""")
    p.add_argument('-b', '--nvme-ctrlr-name', help='nvme ctrlr name', required=True)
    p.add_argument('-n', '--nsid', help='namespace ID (only support nsid=1 for now)', type=int, required=True)
    p.add_argument('-i', '--locking-range-id', help='locking range id', type=int, required=True)
    p.add_argument('-s', '--range-start', help='locking range start LBA', type=int, required=True)
    p.add_argument('-l', '--range-length', help='locking range length (in blocks)', type=int, required=True)
    p.add_argument('-p', '--password', help='admin password', required=True)
    p.set_defaults(func=bdev_opal_create)

    def bdev_opal_get_info(args):
        print_dict(rpc.bdev.bdev_opal_get_info(args.client,
                                               bdev_name=args.bdev_name,
                                               password=args.password))

    p = subparsers.add_parser('bdev_opal_get_info', help='get opal locking range info for this bdev')
    p.add_argument('-b', '--bdev-name', help='opal bdev')
    p.add_argument('-p', '--password', help='password')
    p.set_defaults(func=bdev_opal_get_info)

    def bdev_opal_delete(args):
        rpc.bdev.bdev_opal_delete(args.client,
                                  bdev_name=args.bdev_name,
                                  password=args.password)

    p = subparsers.add_parser('bdev_opal_delete', help="""delete a virtual opal bdev""")
    p.add_argument('-b', '--bdev-name', help='opal virtual bdev', required=True)
    p.add_argument('-p', '--password', help='admin password', required=True)
    p.set_defaults(func=bdev_opal_delete)

    def bdev_opal_new_user(args):
        rpc.bdev.bdev_opal_new_user(args.client,
                                    bdev_name=args.bdev_name,
                                    admin_password=args.admin_password,
                                    user_id=args.user_id,
                                    user_password=args.user_password)

    p = subparsers.add_parser('bdev_opal_new_user', help="""Add a user to opal bdev who can set lock state for this bdev""")
    p.add_argument('-b', '--bdev-name', help='opal bdev', required=True)
    p.add_argument('-p', '--admin-password', help='admin password', required=True)
    p.add_argument('-i', '--user-id', help='ID for new user', type=int, required=True)
    p.add_argument('-u', '--user-password', help='password set for this user', required=True)
    p.set_defaults(func=bdev_opal_new_user)

    def bdev_opal_set_lock_state(args):
        rpc.bdev.bdev_opal_set_lock_state(args.client,
                                          bdev_name=args.bdev_name,
                                          user_id=args.user_id,
                                          password=args.password,
                                          lock_state=args.lock_state)

    p = subparsers.add_parser('bdev_opal_set_lock_state', help="""set lock state for an opal bdev""")
    p.add_argument('-b', '--bdev-name', help='opal bdev', required=True)
    p.add_argument('-i', '--user-id', help='ID of the user who want to set lock state, either admin or a user assigned to this bdev',
                   type=int, required=True)
    p.add_argument('-p', '--password', help='password of this user', required=True)
    p.add_argument('-l', '--lock-state', help='lock state to set, choose from {readwrite, readonly, rwlock}', required=True)
    p.set_defaults(func=bdev_opal_set_lock_state)

    # bdev_nvme_send_cmd
    def bdev_nvme_send_cmd(args):
        print_dict(rpc.nvme.bdev_nvme_send_cmd(args.client,
                                               name=args.nvme_name,
                                               cmd_type=args.cmd_type,
                                               data_direction=args.data_direction,
                                               cmdbuf=args.cmdbuf,
                                               data=args.data,
                                               metadata=args.metadata,
                                               data_len=args.data_length,
                                               metadata_len=args.metadata_length,
                                               timeout_ms=args.timeout_ms))

    p = subparsers.add_parser('bdev_nvme_send_cmd', help='NVMe passthrough cmd.')
    p.add_argument('-n', '--nvme-name', help="""Name of the operating NVMe controller""")
    p.add_argument('-t', '--cmd-type', help="""Type of nvme cmd. Valid values are: admin, io""")
    p.add_argument('-r', '--data-direction', help="""Direction of data transfer. Valid values are: c2h, h2c""")
    p.add_argument('-c', '--cmdbuf', help="""NVMe command encoded by base64 urlsafe""")
    p.add_argument('-d', '--data', help="""Data transferring to controller from host, encoded by base64 urlsafe""")
    p.add_argument('-m', '--metadata', help="""Metadata transferring to controller from host, encoded by base64 urlsafe""")
    p.add_argument('-D', '--data-length', help="""Data length required to transfer from controller to host""", type=int)
    p.add_argument('-M', '--metadata-length', help="""Metadata length required to transfer from controller to host""", type=int)
    p.add_argument('-T', '--timeout-ms',
                   help="""Command execution timeout value, in milliseconds,  if 0, don't track timeout""", type=int)
    p.set_defaults(func=bdev_nvme_send_cmd)

    # Notifications
    def notify_get_types(args):
        print_dict(rpc.notify.notify_get_types(args.client))

    p = subparsers.add_parser('notify_get_types', help='List available notifications that user can subscribe to.')
    p.set_defaults(func=notify_get_types)

    def notify_get_notifications(args):
        ret = rpc.notify.notify_get_notifications(args.client,
                                                  id=args.id,
                                                  max=args.max)
        print_dict(ret)

    p = subparsers.add_parser('notify_get_notifications', help='Get notifications')
    p.add_argument('-i', '--id', help="""First ID to start fetching from""", type=int)
    p.add_argument('-n', '--max', help="""Maximum number of notifications to return in response""", type=int)
    p.set_defaults(func=notify_get_notifications)

    def thread_get_stats(args):
        print_dict(rpc.app.thread_get_stats(args.client))

    p = subparsers.add_parser(
        'thread_get_stats', help='Display current statistics of all the threads')
    p.set_defaults(func=thread_get_stats)

    def thread_set_cpumask(args):
        ret = rpc.app.thread_set_cpumask(args.client,
                                         id=args.id,
                                         cpumask=args.cpumask)
    p = subparsers.add_parser('thread_set_cpumask',
                              help="""set the cpumask of the thread whose ID matches to the
    specified value. The thread may be migrated to one of the specified CPUs.""")
    p.add_argument('-i', '--id', type=int, help='thread ID')
    p.add_argument('-m', '--cpumask', help='cpumask for this thread')
    p.set_defaults(func=thread_set_cpumask)

    def log_enable_timestamps(args):
        ret = rpc.app.log_enable_timestamps(args.client,
                                            enabled=args.enabled)
    p = subparsers.add_parser('log_enable_timestamps',
                              help='Enable or disable timestamps.')
    p.add_argument('-d', '--disable', dest='enabled', default=False, action='store_false', help="Disable timestamps")
    p.add_argument('-e', '--enable', dest='enabled', action='store_true', help="Enable timestamps")
    p.set_defaults(func=log_enable_timestamps)

    def thread_get_pollers(args):
        print_dict(rpc.app.thread_get_pollers(args.client))

    p = subparsers.add_parser(
        'thread_get_pollers', help='Display current pollers of all the threads')
    p.set_defaults(func=thread_get_pollers)

    def thread_get_io_channels(args):
        print_dict(rpc.app.thread_get_io_channels(args.client))

    p = subparsers.add_parser(
        'thread_get_io_channels', help='Display current IO channels of all the threads')
    p.set_defaults(func=thread_get_io_channels)

    def env_dpdk_get_mem_stats(args):
        print_dict(rpc.env_dpdk.env_dpdk_get_mem_stats(args.client))

    p = subparsers.add_parser(
        'env_dpdk_get_mem_stats', help='write the dpdk memory stats to a file.')
    p.set_defaults(func=env_dpdk_get_mem_stats)

    # blobfs
    def blobfs_detect(args):
        print(rpc.blobfs.blobfs_detect(args.client,
                                       bdev_name=args.bdev_name))

    p = subparsers.add_parser('blobfs_detect', help='Detect whether a blobfs exists on bdev')
    p.add_argument('bdev_name', help='Blockdev name to detect blobfs. Example: Malloc0.')
    p.set_defaults(func=blobfs_detect)

    def blobfs_create(args):
        print(rpc.blobfs.blobfs_create(args.client,
                                       bdev_name=args.bdev_name,
                                       cluster_sz=args.cluster_sz))

    p = subparsers.add_parser('blobfs_create', help='Build a blobfs on bdev')
    p.add_argument('bdev_name', help='Blockdev name to build blobfs. Example: Malloc0.')
    p.add_argument('-c', '--cluster-sz',
                   help="""Size of cluster in bytes (Optional). Must be multiple of 4KB page size. Default and minimal value is 1M.""")
    p.set_defaults(func=blobfs_create)

    def blobfs_mount(args):
        print(rpc.blobfs.blobfs_mount(args.client,
                                      bdev_name=args.bdev_name,
                                      mountpoint=args.mountpoint))

    p = subparsers.add_parser('blobfs_mount', help='Mount a blobfs on bdev to host path by FUSE')
    p.add_argument('bdev_name', help='Blockdev name where the blobfs is. Example: Malloc0.')
    p.add_argument('mountpoint', help='Mountpoint path in host to mount blobfs. Example: /mnt/.')
    p.set_defaults(func=blobfs_mount)

    def blobfs_set_cache_size(args):
        print(rpc.blobfs.blobfs_set_cache_size(args.client,
                                               size_in_mb=args.size_in_mb))

    p = subparsers.add_parser('blobfs_set_cache_size', help='Set cache size for blobfs')
    p.add_argument('size_in_mb', help='Cache size for blobfs in megabytes.', type=int)
    p.set_defaults(func=blobfs_set_cache_size)

    # sock
    def sock_impl_get_options(args):
        print_json(rpc.sock.sock_impl_get_options(args.client,
                                                  impl_name=args.impl))

    p = subparsers.add_parser('sock_impl_get_options', help="""Get options of socket layer implementation""")
    p.add_argument('-i', '--impl', help='Socket implementation name, e.g. posix', required=True)
    p.set_defaults(func=sock_impl_get_options)

    def sock_impl_set_options(args):
        rpc.sock.sock_impl_set_options(args.client,
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
    p.add_argument('--enable-recv-pipe', help='Enable receive pipe',
                   action='store_true', dest='enable_recv_pipe')
    p.add_argument('--disable-recv-pipe', help='Disable receive pipe',
                   action='store_false', dest='enable_recv_pipe')
    p.add_argument('--enable-quickack', help='Enable quick ACK',
                   action='store_true', dest='enable_quickack')
    p.add_argument('--disable-quickack', help='Disable quick ACK',
                   action='store_false', dest='enable_quickack')
    p.add_argument('--enable-zerocopy-send-server', help='Enable zerocopy on send for server sockets',
                   action='store_true', dest='enable_zerocopy_send_server')
    p.add_argument('--disable-zerocopy-send-server', help='Disable zerocopy on send for server sockets',
                   action='store_false', dest='enable_zerocopy_send_server')
    p.add_argument('--enable-zerocopy-send-client', help='Enable zerocopy on send for client sockets',
                   action='store_true', dest='enable_zerocopy_send_client')
    p.add_argument('--disable-zerocopy-send-client', help='Disable zerocopy on send for client sockets',
                   action='store_false', dest='enable_zerocopy_send_client')
    p.add_argument('--zerocopy-threshold', help='Set zerocopy_threshold in bytes', type=int)
    p.add_argument('--tls-version', help='TLS protocol version', type=int)
    p.add_argument('--enable-ktls', help='Enable Kernel TLS',
                   action='store_true', dest='enable_ktls')
    p.add_argument('--disable-ktls', help='Disable Kernel TLS',
                   action='store_false', dest='enable_ktls')
    p.set_defaults(func=sock_impl_set_options, enable_recv_pipe=None, enable_quickack=None,
                   enable_placement_id=None, enable_zerocopy_send_server=None, enable_zerocopy_send_client=None,
                   zerocopy_threshold=None, tls_version=None, enable_ktls=None)

    def sock_set_default_impl(args):
        print_json(rpc.sock.sock_set_default_impl(args.client,
                                                  impl_name=args.impl))

    p = subparsers.add_parser('sock_set_default_impl', help="""Set the default sock implementation""")
    p.add_argument('-i', '--impl', help='Socket implementation name, e.g. posix', required=True)
    p.set_defaults(func=sock_set_default_impl)

    def framework_get_pci_devices(args):
        def splitbuf(buf, step):
            return [buf[i:i+step] for i in range(0, len(buf), step)]

        devices = rpc.subsystem.framework_get_pci_devices(args.client)
        if not args.format_lspci:
            print_json(devices)
        else:
            for devid, dev in enumerate(devices):
                print('{} device #{}'.format(dev['address'], devid))
                for lineid, line in enumerate(splitbuf(dev['config_space'], 32)):
                    print('{:02x}: {}'.format(lineid * 16, ' '.join(splitbuf(line.lower(), 2))))
                print()

    p = subparsers.add_parser('framework_get_pci_devices', help='''Get a list of attached PCI devices''')
    p.add_argument('--format-lspci', help='Format the output in a way to be consumed by lspci -F',
                   action='store_true')
    p.set_defaults(func=framework_get_pci_devices)

    # bdev_nvme_add_error_injection
    def bdev_nvme_add_error_injection(args):
        print_dict(rpc.nvme.bdev_nvme_add_error_injection(args.client,
                                                          name=args.nvme_name,
                                                          cmd_type=args.cmd_type,
                                                          opc=args.opc,
                                                          do_not_submit=args.do_not_submit,
                                                          timeout_in_us=args.timeout_in_us,
                                                          err_count=args.err_count,
                                                          sct=args.sct,
                                                          sc=args.sc))
    p = subparsers.add_parser('bdev_nvme_add_error_injection',
                              help='Add a NVMe command error injection.')
    p.add_argument('-n', '--nvme-name', help="""Name of the operating NVMe controller""", required=True)
    p.add_argument('-t', '--cmd-type', help="""Type of NVMe command. Valid values are: admin, io""", required=True)
    p.add_argument('-o', '--opc', help="""Opcode of the NVMe command.""", required=True, type=int)
    p.add_argument('-s', '--do-not-submit',
                   help="""Set to true if request should not be submitted to the controller""",
                   dest="do_not_submit", action='store_true')
    p.add_argument('-w', '--timeout-in-us', help="""Wait specified microseconds when do_not_submit is true""", type=int)
    p.add_argument('-e', '--err-count', help="""Number of matching NVMe commands to inject errors""", type=int)
    p.add_argument('-u', '--sct', help="""Status code type""", type=int)
    p.add_argument('-c', '--sc', help="""Status code""", type=int)
    p.set_defaults(func=bdev_nvme_add_error_injection)

    # bdev_nvme_remove_error_injection
    def bdev_nvme_remove_error_injection(args):
        print_dict(rpc.nvme.bdev_nvme_remove_error_injection(args.client,
                                                             name=args.nvme_name,
                                                             cmd_type=args.cmd_type,
                                                             opc=args.opc))
    p = subparsers.add_parser('bdev_nvme_remove_error_injection',
                              help='Removes a NVMe command error injection.')
    p.add_argument('-n', '--nvme-name', help="""Name of the operating NVMe controller""", required=True)
    p.add_argument('-t', '--cmd-type', help="""Type of nvme cmd. Valid values are: admin, io""", required=True)
    p.add_argument('-o', '--opc', help="""Opcode of the nvme cmd.""", required=True, type=int)
    p.set_defaults(func=bdev_nvme_remove_error_injection)

    def bdev_daos_create(args):
        num_blocks = (args.total_size * 1024 * 1024) // args.block_size
        print_json(rpc.bdev.bdev_daos_create(args.client,
                                             num_blocks=int(num_blocks),
                                             block_size=args.block_size,
                                             name=args.name,
                                             uuid=args.uuid,
                                             pool=args.pool,
                                             cont=args.cont,
                                             oclass=args.oclass))
    p = subparsers.add_parser('bdev_daos_create',
                              help='Create a bdev with DAOS backend')
    p.add_argument('name', help="Name of the bdev")
    p.add_argument('pool', help="UUID of the DAOS pool")
    p.add_argument('cont', help="UUID of the DAOS container")
    p.add_argument(
        'total_size', help='Size of DAOS bdev in MB (float > 0)', type=float)
    p.add_argument('block_size', help='Block size for this bdev', type=int)
    p.add_argument('-u', '--uuid', help="UUID of the bdev")
    p.add_argument('-o', '--oclass', help="DAOS object class")
    p.set_defaults(func=bdev_daos_create)

    def bdev_daos_delete(args):
        rpc.bdev.bdev_daos_delete(args.client,
                                  name=args.name)

    p = subparsers.add_parser('bdev_daos_delete',
                              help='Delete a DAOS disk')
    p.add_argument('name', help='DAOS bdev name')
    p.set_defaults(func=bdev_daos_delete)

    def bdev_daos_resize(args):
        print_json(rpc.bdev.bdev_daos_resize(args.client,
                                             name=args.name,
                                             new_size=int(args.new_size)))

    p = subparsers.add_parser('bdev_daos_resize',
                              help='Resize a DAOS bdev')
    p.add_argument('name', help='DAOS bdev name')
    p.add_argument('new_size', help='new bdev size for resize operation. The unit is MiB')
    p.set_defaults(func=bdev_daos_resize)

    def iobuf_set_options(args):
        rpc.iobuf.iobuf_set_options(args.client,
                                    small_pool_count=args.small_pool_count,
                                    large_pool_count=args.large_pool_count,
                                    small_bufsize=args.small_bufsize,
                                    large_bufsize=args.large_bufsize)
    p = subparsers.add_parser('iobuf_set_options', help='Set iobuf pool options')
    p.add_argument('--small-pool-count', help='number of small buffers in the global pool', type=int)
    p.add_argument('--large-pool-count', help='number of large buffers in the global pool', type=int)
    p.add_argument('--small-bufsize', help='size of a small buffer', type=int)
    p.add_argument('--large-bufsize', help='size of a large buffer', type=int)
    p.set_defaults(func=iobuf_set_options)

    def bdev_nvme_start_mdns_discovery(args):
        rpc.bdev.bdev_nvme_start_mdns_discovery(args.client,
                                                name=args.name,
                                                svcname=args.svcname,
                                                hostnqn=args.hostnqn)

    p = subparsers.add_parser('bdev_nvme_start_mdns_discovery', help='Start mdns based automatic discovery')
    p.add_argument('-b', '--name', help="Name of the NVMe controller prefix for each bdev name", required=True)
    p.add_argument('-s', '--svcname', help='Service type to discover: e.g., _nvme-disc._tcp', required=True)
    p.add_argument('-q', '--hostnqn', help='NVMe-oF host subnqn')
    p.set_defaults(func=bdev_nvme_start_mdns_discovery)

    def bdev_nvme_stop_mdns_discovery(args):
        rpc.bdev.bdev_nvme_stop_mdns_discovery(args.client, name=args.name)

    p = subparsers.add_parser('bdev_nvme_stop_mdns_discovery', help='Stop automatic mdns discovery')
    p.add_argument('-b', '--name', help="Name of the service to stop", required=True)
    p.set_defaults(func=bdev_nvme_stop_mdns_discovery)

    def bdev_nvme_get_mdns_discovery_info(args):
        print_dict(rpc.bdev.bdev_nvme_get_mdns_discovery_info(args.client))

    p = subparsers.add_parser('bdev_nvme_get_mdns_discovery_info', help='Get information about the automatic mdns discovery')
    p.set_defaults(func=bdev_nvme_get_mdns_discovery_info)

    def check_called_name(name):
        if name in deprecated_aliases:
            print("{} is deprecated, use {} instead.".format(name, deprecated_aliases[name]), file=sys.stderr)

    class dry_run_client:
        def call(self, method, params=None):
            print("Request:\n" + json.dumps({"method": method, "params": params}, indent=2))

    def null_print(arg):
        pass

    def call_rpc_func(args):
        args.func(args)
        check_called_name(args.called_rpc_name)

    def execute_script(parser, client, fd):
        executed_rpc = ""
        for rpc_call in map(str.rstrip, fd):
            if not rpc_call.strip():
                continue
            executed_rpc = "\n".join([executed_rpc, rpc_call])
            rpc_args = shlex.split(rpc_call)
            if rpc_args[0][0] == '#':
                # Ignore lines starting with # - treat them as comments
                continue
            args = parser.parse_args(rpc_args)
            args.client = client
            try:
                call_rpc_func(args)
            except JSONRPCException as ex:
                print("Exception:")
                print(executed_rpc.strip() + " <<<")
                print(ex.message)
                exit(1)

    def load_plugin(args):
        # Create temporary parser, pull out the plugin parameter, load the module, and then run the real argument parser
        plugin_parser = argparse.ArgumentParser(add_help=False)
        plugin_parser.add_argument('--plugin', dest='rpc_plugin', help='Module name of plugin with additional RPC commands')

        rpc_module = plugin_parser.parse_known_args()[0].rpc_plugin
        if args is not None:
            rpc_module = plugin_parser.parse_known_args(args)[0].rpc_plugin

        if rpc_module in plugins:
            return

        if rpc_module is not None:
            try:
                rpc_plugin = importlib.import_module(rpc_module)
                try:
                    rpc_plugin.spdk_rpc_plugin_initialize(subparsers)
                    plugins.append(rpc_module)
                except AttributeError:
                    print("Module %s does not contain 'spdk_rpc_plugin_initialize' function" % rpc_module)
            except ModuleNotFoundError:
                print("Module %s not found" % rpc_module)

    def replace_arg_underscores(args):
        # All option names are defined with dashes only - for example: --tgt-name
        # But if user used underscores, convert them to dashes (--tgt_name => --tgt-name)
        # SPDK was inconsistent previously and had some options with underscores, so
        # doing this conversion ensures backward compatibility with older scripts.
        for i in range(len(args)):
            arg = args[i]
            if arg.startswith('--') and "_" in arg:
                opt, *vals = arg.split('=')
                args[i] = '='.join([opt.replace('_', '-'), *vals])

    plugins = []
    load_plugin(None)

    replace_arg_underscores(sys.argv)

    args = parser.parse_args()

    if sys.stdin.isatty() and not hasattr(args, 'func'):
        # No arguments and no data piped through stdin
        parser.print_help()
        exit(1)
    if args.is_server:
        for input in sys.stdin:
            cmd = shlex.split(input)
            replace_arg_underscores(cmd)
            try:
                load_plugin(cmd)
                tmp_args = parser.parse_args(cmd)
            except SystemExit as ex:
                print("**STATUS=1", flush=True)
                continue

            try:
                tmp_args.client = rpc.client.JSONRPCClient(
                    tmp_args.server_addr, tmp_args.port, tmp_args.timeout,
                    log_level=getattr(logging, tmp_args.verbose.upper()), conn_retries=tmp_args.conn_retries)
                call_rpc_func(tmp_args)
                print("**STATUS=0", flush=True)
            except JSONRPCException as ex:
                print(ex.message)
                print("**STATUS=1", flush=True)
        exit(0)
    elif args.dry_run:
        args.client = dry_run_client()
        print_dict = null_print
        print_json = null_print
        print_array = null_print
    else:
        try:
            args.client = rpc.client.JSONRPCClient(args.server_addr, args.port, args.timeout,
                                                   log_level=getattr(logging, args.verbose.upper()),
                                                   conn_retries=args.conn_retries)
        except JSONRPCException as ex:
            print(ex.message)
            exit(1)

    if hasattr(args, 'func'):
        try:
            call_rpc_func(args)
        except JSONRPCException as ex:
            print(ex.message)
            exit(1)
    else:
        execute_script(parser, args.client, sys.stdin)
