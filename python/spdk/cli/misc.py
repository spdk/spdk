#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2016 Intel Corporation
#  All rights reserved.
#  Copyright (c) 2022 Dell Inc, or its subsidiaries.
#  Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
#

import argparse
import sys

from spdk.rpc import config
from spdk.rpc.cmd_parser import print_dict, print_json


def add_parser(subparsers):

    def spdk_get_version(args):
        print_json(args.client.spdk_get_version())

    p = subparsers.add_parser('spdk_get_version', help='Get SPDK version')
    p.set_defaults(func=spdk_get_version)

    def save_config(args):
        config.save_config(args.client,
                           fd=sys.stdout,
                           indent=args.indent,
                           subsystems=args.subsystems,
                           with_batches=args.with_batches)

    p = subparsers.add_parser('save_config', help="""Write current (live) configuration of SPDK subsystems and targets to stdout.
    """)
    p.add_argument('-i', '--indent', help="""Indent level. Value less than 0 mean compact mode. Default indent level is 2.
    """, type=int, default=2)
    p.add_argument('-s', '--subsystems', help="""Comma-separated list of subsystems (and their dependencies) to save""")
    p.add_argument('--with-batches', action=argparse.BooleanOptionalAction, default=False,
                   help='Keep batch arrays in output; --no-with-batches flattens them (default)')
    p.set_defaults(func=save_config)

    def load_config(args):
        config.load_config(args.client,
                           fd=args.json_conf,
                           include_aliases=args.include_aliases)

    p = subparsers.add_parser('load_config', help="""Configure SPDK subsystems and targets using JSON RPC.""")
    p.add_argument('-i', '--include-aliases', help='include RPC aliases', action='store_true')
    p.add_argument('-j', '--json-conf', help='Valid JSON configuration', default=sys.stdin)
    p.set_defaults(func=load_config)

    def save_subsystem_config(args):
        config.save_subsystem_config(args.client,
                                     fd=sys.stdout,
                                     indent=args.indent,
                                     name=args.name,
                                     with_batches=args.with_batches)

    p = subparsers.add_parser('save_subsystem_config', help="""Write current (live) configuration of SPDK subsystem to stdout.
    """)
    p.add_argument('-i', '--indent', help="""Indent level. Value less than 0 mean compact mode. Default indent level is 2.
    """, type=int, default=2)
    p.add_argument('-n', '--name', help='Name of subsystem', required=True)
    p.add_argument('--with-batches', action=argparse.BooleanOptionalAction, default=False,
                   help='Keep batch arrays in output; --no-with-batches flattens them (default)')
    p.set_defaults(func=save_subsystem_config)

    def load_subsystem_config(args):
        config.load_subsystem_config(args.client, fd=args.json_conf)

    p = subparsers.add_parser('load_subsystem_config', help="""Configure SPDK subsystem using JSON RPC.""")
    p.add_argument('-j', '--json-conf', help='Valid JSON configuration', default=sys.stdin)
    p.set_defaults(func=load_subsystem_config)

    # subsystem
    def framework_get_subsystems(args):
        print_dict(args.client.framework_get_subsystems())

    p = subparsers.add_parser('framework_get_subsystems',
                              help="""Print subsystems array in initialization order. Each subsystem
    entry contain (unsorted) array of subsystems it depends on.""")
    p.set_defaults(func=framework_get_subsystems)

    def framework_get_config(args):
        print_dict(args.client.framework_get_config(name=args.name, with_batches=args.with_batches))

    p = subparsers.add_parser('framework_get_config', help="""Print subsystem configuration""")
    p.add_argument('name', help='Name of the SPDK subsystem to query')
    p.add_argument('--with-batches', action=argparse.BooleanOptionalAction, default=False,
                   help='Keep batch arrays in output. Default: false (flatten into individual entries)')
    p.set_defaults(func=framework_get_config)

    # ioat
    def ioat_scan_accel_module(args):
        args.client.ioat_scan_accel_module()

    p = subparsers.add_parser('ioat_scan_accel_module',
                              help='Enable IOAT accel module offload.')
    p.set_defaults(func=ioat_scan_accel_module)

    # dpdk compressdev
    def compressdev_scan_accel_module(args):
        args.client.compressdev_scan_accel_module(pmd=args.pmd)

    p = subparsers.add_parser('compressdev_scan_accel_module', help='Scan and enable compressdev module and set pmd option.')
    p.add_argument('-p', '--pmd', type=int, help='PMD selection: 0=auto, 1=QAT only, 2=mlx5_pci only, 3=uadk only', required=True)
    p.set_defaults(func=compressdev_scan_accel_module)

    # dsa
    def dsa_scan_accel_module(args):
        args.client.dsa_scan_accel_module(config_kernel_mode=args.config_kernel_mode)

    p = subparsers.add_parser('dsa_scan_accel_module',
                              help='Set config and enable dsa accel module offload.')
    p.add_argument('-k', '--config-kernel-mode', help='Use the kernel idxd driver instead of user-mode. Default: false',
                   action='store_true', dest='config_kernel_mode')
    p.set_defaults(func=dsa_scan_accel_module, config_kernel_mode=None)

    # iaa
    def iaa_scan_accel_module(args):
        args.client.iaa_scan_accel_module()

    p = subparsers.add_parser('iaa_scan_accel_module',
                              help='Set config and enable iaa accel module offload.')
    p.set_defaults(func=iaa_scan_accel_module)

    def dpdk_cryptodev_scan_accel_module(args):
        args.client.dpdk_cryptodev_scan_accel_module()

    p = subparsers.add_parser('dpdk_cryptodev_scan_accel_module',
                              help='Enable dpdk_cryptodev accel module offload.')
    p.set_defaults(func=dpdk_cryptodev_scan_accel_module)

    def dpdk_cryptodev_set_driver(args):
        args.client.dpdk_cryptodev_set_driver(driver_name=args.driver_name)

    p = subparsers.add_parser('dpdk_cryptodev_set_driver',
                              help='Set the DPDK cryptodev driver.')
    p.add_argument('-d', '--driver-name',
                   help='DPDK crypto driver: crypto_aesni_mb, crypto_qat, crypto_uadk, mlx5_pci',
                   choices=['crypto_aesni_mb', 'crypto_qat', 'mlx5_pci', 'crypto_uadk'],
                   required=True)
    p.set_defaults(func=dpdk_cryptodev_set_driver)

    def dpdk_cryptodev_get_driver(args):
        print_dict(args.client.dpdk_cryptodev_get_driver())

    p = subparsers.add_parser('dpdk_cryptodev_get_driver', help='Get the DPDK cryptodev driver')
    p.set_defaults(func=dpdk_cryptodev_get_driver)

    # ae4dma
    def ae4dma_scan_accel_module(args):
        args.client.ae4dma_scan_accel_module()

    p = subparsers.add_parser('ae4dma_scan_accel_module',
                              help='Enable AE4DMA accel module offload.')
    p.set_defaults(func=ae4dma_scan_accel_module)

    # mlx5
    def mlx5_scan_accel_module(args):
        args.client.mlx5_scan_accel_module(
                                        qp_size=args.qp_size,
                                        num_requests=args.num_requests,
                                        allowed_devs=args.allowed_devs,
                                        crypto_split_blocks=args.crypto_split_blocks,
                                        enable_driver=args.enable_driver)

    p = subparsers.add_parser('mlx5_scan_accel_module', help='Enable mlx5 accel module.')
    p.add_argument('-q', '--qp-size', type=int, help='Queue pair size')
    p.add_argument('-r', '--num-requests', type=int, help='Size of the shared requests pool')
    p.add_argument('-d', '--allowed-devs', help="Comma separated list of allowed device names, e.g. mlx5_0,mlx5_1")
    p.add_argument('-s', '--crypto-split-blocks', type=int,
                   help="Number of data blocks to be processed in 1 crypto UMR. [0-65535], 0 means no limit")
    p.add_argument('-e', '--enable-driver', dest='enable_driver', action='store_true', default=None,
                   help="Enable mlx5 platform driver. Note: the driver supports reduced scope of operations, enable with care")
    p.set_defaults(func=mlx5_scan_accel_module)

    def accel_mlx5_dump_stats(args):
        print_dict(args.client.accel_mlx5_dump_stats(level=args.level))

    p = subparsers.add_parser('accel_mlx5_dump_stats', help='Dump accel mlx5 module statistics.')
    p.add_argument('-l', '--level',
                   choices=['total', 'channel', 'device'],
                   help='Verbosity level: total, channel, device. Default: channel')
    p.set_defaults(func=accel_mlx5_dump_stats)

    # cuda
    def cuda_scan_accel_module(args):
        args.client.cuda_scan_accel_module()

    p = subparsers.add_parser('cuda_scan_accel_module', help='Enable CUDA accel module offload.')
    p.set_defaults(func=cuda_scan_accel_module)

    # accel_error
    def accel_error_inject_error(args):
        args.client.accel_error_inject_error(opcode=args.opcode,
                                             type=args.type, count=args.count,
                                             interval=args.interval, errcode=args.errcode)

    p = subparsers.add_parser('accel_error_inject_error',
                              help='Inject an error to processing accel operation')
    p.add_argument('-o', '--opcode', required=True,
                   choices=['copy', 'fill', 'dualcast', 'compare', 'crc32c', 'copy_crc32c',
                            'compress', 'decompress', 'encrypt', 'decrypt', 'xor',
                            'dif_verify', 'dif_verify_copy', 'dif_generate', 'dif_generate_copy',
                            'dix_generate', 'dix_verify'],
                   help='Accel operation to inject errors into')
    p.add_argument('-t', '--type', required=True,
                   choices=['disable', 'corrupt', 'failure'],
                   help='Injection type: corrupt (corrupt data), failure (fail operation), disable (disable injection)')
    p.add_argument('-c', '--count', type=int,
                   help='Number of errors to inject on each IO channel; 0 disables injection. Default: UINT64_MAX (no limit)')
    p.add_argument('-i', '--interval', type=int, help='Number of operations between successive injections')
    p.add_argument('--errcode', type=int, help='Error code to return; only relevant for type=failure')
    p.set_defaults(func=accel_error_inject_error)

    def env_dpdk_get_mem_stats(args):
        print_dict(args.client.env_dpdk_get_mem_stats())

    p = subparsers.add_parser(
        'env_dpdk_get_mem_stats', help='write the dpdk memory stats to a file.')
    p.set_defaults(func=env_dpdk_get_mem_stats)

    def framework_get_pci_devices(args):
        def splitbuf(buf, step):
            return [buf[i:i+step] for i in range(0, len(buf), step)]

        devices = args.client.framework_get_pci_devices()
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
