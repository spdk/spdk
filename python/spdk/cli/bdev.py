#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2016 Intel Corporation
#  All rights reserved.
#  Copyright (c) 2022 Dell Inc, or its subsidiaries.
#  Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
#

import sys
import spdk.rpc as rpc  # noqa
from spdk.rpc.client import print_dict, print_json, print_array  # noqa


def add_parser(subparsers):

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

    def bdev_set_options(args):
        rpc.bdev.bdev_set_options(args.client,
                                  bdev_io_pool_size=args.bdev_io_pool_size,
                                  bdev_io_cache_size=args.bdev_io_cache_size,
                                  bdev_auto_examine=args.bdev_auto_examine,
                                  iobuf_small_cache_size=args.iobuf_small_cache_size,
                                  iobuf_large_cache_size=args.iobuf_large_cache_size)

    p = subparsers.add_parser('bdev_set_options',
                              help="""Set options of bdev subsystem""")
    p.add_argument('-p', '--bdev-io-pool-size', help='Number of bdev_io structures in shared buffer pool', type=int)
    p.add_argument('-c', '--bdev-io-cache-size', help='Maximum number of bdev_io structures cached per thread', type=int)
    group = p.add_mutually_exclusive_group()
    group.add_argument('-e', '--enable-auto-examine', dest='bdev_auto_examine', help='Allow to auto examine', action='store_true')
    group.add_argument('-d', '--disable-auto-examine', dest='bdev_auto_examine', help='Not allow to auto examine', action='store_false')
    p.add_argument('--iobuf-small-cache-size', help='Size of the small iobuf per thread cache', type=int)
    p.add_argument('--iobuf-large-cache-size', help='Size of the large iobuf per thread cache', type=int)
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
        print("bdev_compress_create RPC is deprecated", file=sys.stderr)
        print_json(rpc.bdev.bdev_compress_create(args.client,
                                                 base_bdev_name=args.base_bdev_name,
                                                 pm_path=args.pm_path,
                                                 lb_size=args.lb_size,
                                                 comp_algo=args.comp_algo,
                                                 comp_level=args.comp_level))

    p = subparsers.add_parser('bdev_compress_create', help='Add a compress vbdev')
    p.add_argument('-b', '--base-bdev-name', help="Name of the base bdev", required=True)
    p.add_argument('-p', '--pm-path', help="Path to persistent memory", required=True)
    p.add_argument('-l', '--lb-size', help="Compressed vol logical block size (optional, if used must be 512 or 4096)", type=int)
    p.add_argument('-c', '--comp-algo', help='Compression algorithm, (deflate, lz4). Default is deflate')
    p.add_argument('-L', '--comp-level',
                   help="""Compression algorithm level.
                   if algo == deflate, level ranges from 0 to 3.
                   if algo == lz4, level ranges from 1 to 65537""",
                   default=1, type=int)
    p.set_defaults(func=bdev_compress_create)

    def bdev_compress_delete(args):
        print("bdev_compress_delete RPC is deprecated", file=sys.stderr)
        rpc.bdev.bdev_compress_delete(args.client,
                                      name=args.name)

    p = subparsers.add_parser('bdev_compress_delete', help='Delete a compress disk')
    p.add_argument('name', help='compress bdev name')
    p.set_defaults(func=bdev_compress_delete)

    def bdev_compress_get_orphans(args):
        print("bdev_compress_get_orphans RPC is deprecated", file=sys.stderr)
        print_dict(rpc.bdev.bdev_compress_get_orphans(args.client,
                                                      name=args.name))
    p = subparsers.add_parser(
        'bdev_compress_get_orphans', help='Display list of orphaned compress bdevs.')
    p.add_argument('-b', '--name', help="Name of a comp bdev. Example: COMP_Nvme0n1")
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
    p.add_argument('-p', '--crypto-pmd', help="Name of the crypto device driver. Obsolete, see dpdk_cryptodev_set_driver")
    p.add_argument('-k', '--key', help="Key. Obsolete, see accel_crypto_key_create")
    p.add_argument('-c', '--cipher', help="cipher to use. Obsolete, see accel_crypto_key_create")
    p.add_argument('-k2', '--key2', help="2nd key for cipher AES_XTS. Obsolete, see accel_crypto_key_create", default=None)
    p.add_argument('-n', '--key-name', help="Key name to use, see accel_crypto_key_create")
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
        choices=[4, 8, 16, 32, 64]
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
                                               dif_is_head_of_md=args.dif_is_head_of_md,
                                               dif_pi_format=args.dif_pi_format))
    p = subparsers.add_parser('bdev_malloc_create', help='Create a bdev with malloc backend')
    p.add_argument('-b', '--name', help="Name of the bdev")
    p.add_argument('-u', '--uuid', help="UUID of the bdev (optional)")
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
    p.add_argument('-f', '--dif-pi-format', type=int, choices=[0, 1, 2],
                   help='Protection infromation format. Parameter --dif-type needs to be set together.'
                        '0=16b Guard PI, 1=32b Guard PI, 2=64b Guard PI. Default=0.')
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
                                             dif_is_head_of_md=args.dif_is_head_of_md,
                                             dif_pi_format=args.dif_pi_format))

    p = subparsers.add_parser('bdev_null_create', help='Add a bdev with null backend')
    p.add_argument('name', help='Block device name')
    p.add_argument('-u', '--uuid', help='UUID of the bdev (optional)')
    p.add_argument('total_size', help='Size of null bdev in MB (int > 0). Includes only data blocks.', type=int)
    p.add_argument('block_size', help='Data block size for this bdev.', type=int)
    p.add_argument('-p', '--physical-block-size', help='Physical block size for this bdev.', type=int)
    p.add_argument('-m', '--md-size', type=int,
                   help='Metadata size for this bdev. Default=0.')
    p.add_argument('-t', '--dif-type', type=int, choices=[0, 1, 2, 3],
                   help='Protection information type. Parameter --md-size needs'
                        'to be set along --dif-type. Default=0 - no protection.')
    p.add_argument('-d', '--dif-is-head-of-md', action='store_true',
                   help='Protection information is in the first 8 bytes of metadata. Default=false.')
    p.add_argument('-f', '--dif-pi-format', type=int, choices=[0, 1, 2],
                   help='Protection infromation format. Parameter --dif-type needs to be set together.'
                        '0=16b Guard PI, 1=32b Guard PI, 2=64b Guard PI. Default=0.')
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
                                            readonly=args.readonly,
                                            fallocate=args.fallocate,
                                            uuid=args.uuid))

    p = subparsers.add_parser('bdev_aio_create', help='Add a bdev with aio backend')
    p.add_argument('filename', help='Path to device or file (ex: /dev/sda)')
    p.add_argument('name', help='Block device name')
    p.add_argument('block_size', help='Block size for this bdev', type=int, nargs='?')
    p.add_argument("-r", "--readonly", action='store_true', help='Set this bdev as read-only')
    p.add_argument("--fallocate", action='store_true', help='Support unmap/writezeros by fallocate')
    p.add_argument('-u', '--uuid', help="UUID of the bdev (optional)")
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
                                              block_size=args.block_size,
                                              uuid=args.uuid))

    p = subparsers.add_parser('bdev_uring_create', help='Create a bdev with io_uring backend')
    p.add_argument('filename', help='Path to device or file (ex: /dev/nvme0n1)')
    p.add_argument('name', help='bdev name')
    p.add_argument('block_size', help='Block size for this bdev', type=int, nargs='?')
    p.add_argument('-u', '--uuid', help="UUID of the bdev")
    p.set_defaults(func=bdev_uring_create)

    def bdev_uring_rescan(args):
        print_json(rpc.bdev.bdev_uring_rescan(args.client,
                                              name=args.name))

    p = subparsers.add_parser('bdev_uring_rescan', help='Rescan a bdev size with uring backend')
    p.add_argument('name', help='Block device name')
    p.set_defaults(func=bdev_uring_rescan)

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
                                              io_mechanism=args.io_mechanism,
                                              conserve_cpu=args.conserve_cpu))

    p = subparsers.add_parser('bdev_xnvme_create', help='Create a bdev with xNVMe backend')
    p.add_argument('filename', help='Path to device or file (ex: /dev/nvme0n1)')
    p.add_argument('name', help='name of xNVMe bdev to create')
    p.add_argument('io_mechanism', help='IO mechanism to use (ex: libaio, io_uring, io_uring_cmd, etc.)')
    p.add_argument('-c', '--conserve-cpu', action='store_true', help='Whether or not to conserve CPU when polling')
    p.set_defaults(func=bdev_xnvme_create)

    def bdev_xnvme_delete(args):
        rpc.bdev.bdev_xnvme_delete(args.client,
                                   name=args.name)

    p = subparsers.add_parser('bdev_xnvme_delete', help='Delete a xNVMe bdev')
    p.add_argument('name', help='xNVMe bdev name')
    p.set_defaults(func=bdev_xnvme_delete)

    def bdev_nvme_set_options(args):
        rpc.bdev.bdev_nvme_set_options(**vars(args))

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
    p.add_argument('--allow-accel-sequence',
                   help='''Allow NVMe bdevs to advertise support for accel sequences if the
                   controller also supports them.''', action='store_true')
    p.add_argument('--rdma-max-cq-size',
                   help='The maximum size of a rdma completion queue. Default: 0 (unlimited)', type=int)
    p.add_argument('--rdma-cm-event-timeout-ms',
                   help='Time to wait for RDMA CM event. Only applicable for RDMA transports.', type=int)
    p.add_argument('--dhchap-digests', help='Comma-separated list of allowed DH-HMAC-CHAP digests',
                   type=lambda d: d.split(','))
    p.add_argument('--dhchap-dhgroups', help='Comma-separated list of allowed DH-HMAC-CHAP DH groups',
                   type=lambda d: d.split(','))
    p.add_argument('--enable-rdma-umr-per-io',
                   help='''Enable scatter-gather RDMA Memory Region per IO if supported by the system.''',
                   action='store_true', dest='rdma_umr_per_io')
    p.add_argument('--disable-rdma-umr-per-io',
                   help='''Disable scatter-gather RDMA Memory Region per IO.''',
                   action='store_false', dest='rdma_umr_per_io')
    p.add_argument('--tcp-connect-timeout-ms',
                   help='Time to wait until TCP connection is done. Default: 0 (no timeout).', type=int)
    p.add_argument('--enable-flush', help='Pass flush to NVMe when volatile write cache is present',
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
                                                         fabrics_connect_timeout_us=args.fabrics_connect_timeout_us,
                                                         multipath=args.multipath,
                                                         num_io_queues=args.num_io_queues,
                                                         ctrlr_loss_timeout_sec=args.ctrlr_loss_timeout_sec,
                                                         reconnect_delay_sec=args.reconnect_delay_sec,
                                                         fast_io_fail_timeout_sec=args.fast_io_fail_timeout_sec,
                                                         psk=args.psk,
                                                         max_bdevs=args.max_bdevs,
                                                         dhchap_key=args.dhchap_key,
                                                         dhchap_ctrlr_key=args.dhchap_ctrlr_key,
                                                         allow_unrecognized_csi=args.allow_unrecognized_csi))

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
    p.add_argument('--fabrics-timeout', type=int, help='Fabrics connect timeout in microseconds',
                   dest="fabrics_connect_timeout_us")
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
                   help="""Set PSK and enable TCP SSL socket implementation.  The PSK can either be a
                   name of a key attached to the keyring or a path to a file containing the key.  The
                   latter method is deprecated.""")
    p.add_argument('-m', '--max-bdevs', type=int,
                   help='The size of the name array for newly created bdevs. Default is 128',)
    p.add_argument('--dhchap-key', help='DH-HMAC-CHAP key name')
    p.add_argument('--dhchap-ctrlr-key', help='DH-HMAC-CHAP controller key name')
    p.add_argument('-U', '--allow-unrecognized-csi', help="""Allow attaching namespaces with unrecognized command set identifiers.
                   These will only support NVMe passthrough.""", action='store_true')

    p.set_defaults(func=bdev_nvme_attach_controller)

    def bdev_nvme_get_controllers(args):
        print_dict(rpc.nvme.bdev_nvme_get_controllers(args.client,
                                                      name=args.name))

    p = subparsers.add_parser(
        'bdev_nvme_get_controllers', help='Display current NVMe controllers list or required NVMe controller')
    p.add_argument('-n', '--name', help="Name of the NVMe controller. Example: Nvme0")
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
        rpc.bdev.bdev_nvme_reset_controller(args.client,
                                            name=args.name,
                                            cntlid=args.cntlid)

    p = subparsers.add_parser('bdev_nvme_reset_controller',
                              help='Reset an NVMe controller or all NVMe controllers in an NVMe bdev controller')
    p.add_argument('name', help="Name of the NVMe controller")
    p.add_argument('-c', '--cntlid', help="NVMe controller ID", type=int)
    p.set_defaults(func=bdev_nvme_reset_controller)

    def bdev_nvme_enable_controller(args):
        rpc.bdev.bdev_nvme_enable_controller(args.client,
                                             name=args.name,
                                             cntlid=args.cntlid)

    p = subparsers.add_parser('bdev_nvme_enable_controller',
                              help='Enable an NVMe controller or all NVMe controllers in an NVMe bdev controller')
    p.add_argument('name', help="Name of the NVMe controller")
    p.add_argument('-c', '--cntlid', help="NVMe controller ID", type=int)
    p.set_defaults(func=bdev_nvme_enable_controller)

    def bdev_nvme_disable_controller(args):
        rpc.bdev.bdev_nvme_disable_controller(args.client,
                                              name=args.name,
                                              cntlid=args.cntlid)

    p = subparsers.add_parser('bdev_nvme_disable_controller',
                              help='Disable an NVMe controller or all NVMe controllers in an NVMe bdev controller')
    p.add_argument('name', help="Name of the NVMe controller")
    p.add_argument('-c', '--cntlid', help="NVMe controller ID", type=int)
    p.set_defaults(func=bdev_nvme_disable_controller)

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
    p.add_argument('-T', '--attach-timeout-ms', type=int,
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
    p.add_argument('-n', '--name', help="Name of the NVMe bdev")
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
    p.add_argument('-s', '--selector', help='Multipath selector (round_robin, queue_depth)')
    p.add_argument('-r', '--rr-min-io',
                   help='Number of IO to route to a path before switching to another for round-robin',
                   type=int)
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

    def bdev_nvme_set_keys(args):
        rpc.bdev.bdev_nvme_set_keys(args.client, args.name, args.dhchap_key, args.dhchap_ctrlr_key)

    p = subparsers.add_parser('bdev_nvme_set_keys',
                              help='Set DH-HMAC-CHAP keys and force (re)authentication on all '
                              'connected qpairs')
    p.add_argument('name', help='Name of the NVMe controller')
    p.add_argument('--dhchap-key', help='DH-HMAC-CHAP key name')
    p.add_argument('--dhchap-ctrlr-key', help='DH-HMAC-CHAP controller key name')
    p.set_defaults(func=bdev_nvme_set_keys)

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
                                                      user_id=args.user,
                                                      config_param=config_param,
                                                      config_file=args.config_file,
                                                      key_file=args.key_file,
                                                      core_mask=args.core_mask))

    p = subparsers.add_parser('bdev_rbd_register_cluster',
                              help='Add a Rados cluster with ceph rbd backend')
    p.add_argument('name', help="Name of the Rados cluster only known to rbd bdev")
    p.add_argument('--user', help="Ceph user name (i.e. admin, not client.admin)")
    p.add_argument('--config-param', action='append', metavar='key=value',
                   help="adds a key=value configuration option for rados_conf_set (default: rely on config file)")
    p.add_argument('--config-file', help="The file path of the Rados configuration file")
    p.add_argument('--key-file', help="The file path of the Rados keyring file")
    p.add_argument('--core-mask', help="Set core mask for librbd IO context threads")
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
    p.add_argument('-b', '--name', help="Name of the registered Rados Cluster Name. Example: Cluster1")
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
                                            user_id=args.user,
                                            config=config,
                                            pool_name=args.pool_name,
                                            rbd_name=args.rbd_name,
                                            block_size=args.block_size,
                                            cluster_name=args.cluster_name,
                                            uuid=args.uuid,
                                            read_only=args.read_only))

    p = subparsers.add_parser('bdev_rbd_create', help='Add a bdev with ceph rbd backend')
    p.add_argument('-b', '--name', help="Name of the bdev")
    p.add_argument('--user', help="Ceph user name (i.e. admin, not client.admin)")
    p.add_argument('--config', action='append', metavar='key=value',
                   help="adds a key=value configuration option for rados_conf_set (default: rely on config file)")
    p.add_argument('pool_name', help='rbd pool name')
    p.add_argument('rbd_name', help='rbd image name')
    p.add_argument('block_size', help='rbd block size', type=int)
    p.add_argument('-c', '--cluster-name', help="cluster name to identify the Rados cluster")
    p.add_argument('-u', '--uuid', help="UUID of the bdev")
    p.add_argument("-r", "--readonly", action='store_true', help='Set this bdev as read-only')
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
    p.add_argument('--uuid', help='UUID for this bdev')
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
                                                 name=args.name,
                                                 uuid=args.uuid))

    p = subparsers.add_parser('bdev_passthru_create', help='Add a pass through bdev on existing bdev')
    p.add_argument('-b', '--base-bdev-name', help="Name of the existing bdev", required=True)
    p.add_argument('-p', '--name', help="Name of the pass through bdev", required=True)
    p.add_argument('-u', '--uuid', help="UUID of the bdev")
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
    p.add_argument('-b', '--name', help="Name of the Blockdev. Example: Nvme0n1")
    p.add_argument('-t', '--timeout-ms', help="""Time in ms to wait for the bdev to appear (only used
    with the -b|--name option). The default timeout is 0, meaning the RPC returns immediately
    whether the bdev exists or not.""",
                   type=int)
    p.set_defaults(func=bdev_get_bdevs)

    def bdev_get_iostat(args):
        print_dict(rpc.bdev.bdev_get_iostat(args.client,
                                            name=args.name,
                                            per_channel=args.per_channel,
                                            reset_mode=args.reset_mode))

    p = subparsers.add_parser('bdev_get_iostat',
                              help='Display current I/O statistics of all the blockdevs or specified blockdev.')
    p.add_argument('-b', '--name', help="Name of the Blockdev. Example: Nvme0n1")
    p.add_argument('-c', '--per-channel', default=False, dest='per_channel', help='Display per channel IO stats for specified device',
                   action='store_true')
    p.add_argument('--reset-mode', help="Mode to reset I/O statistics after getting", choices=['all', 'maxmin', 'none'])
    p.set_defaults(func=bdev_get_iostat)

    def bdev_reset_iostat(args):
        rpc.bdev.bdev_reset_iostat(args.client, name=args.name, mode=args.mode)

    p = subparsers.add_parser('bdev_reset_iostat',
                              help='Reset I/O statistics of all the blockdevs or specified blockdev.')
    p.add_argument('-b', '--name', help="Name of the Blockdev. Example: Nvme0n1")
    p.add_argument('-m', '--mode', help="Mode to reset I/O statistics", choices=['all', 'maxmin', 'none'])
    p.set_defaults(func=bdev_reset_iostat)

    def bdev_enable_histogram(args):
        rpc.bdev.bdev_enable_histogram(args.client, name=args.name, enable=args.enable, opc=args.opc,
                                       granularity=args.granularity, min_nsec=args.min_nsec, max_nsec=args.max_nsec)

    p = subparsers.add_parser('bdev_enable_histogram',
                              help='Enable or disable histogram for specified bdev')
    p.add_argument('-e', '--enable', default=True, dest='enable', action='store_true', help='Enable histograms on specified device')
    p.add_argument('-d', '--disable', dest='enable', action='store_false', help='Disable histograms on specified device')
    p.add_argument('-o', '--opc', help='Enable histogram for specified io type. Defaults to all io types if not specified.'
                   ' Refer to bdev_get_bdevs RPC for the list of io types.')
    p.add_argument('--granularity', help='Histogram bucket granularity.', type=int)
    p.add_argument('--min-nsec', help='Histogram min value in nanoseconds.', type=int)
    p.add_argument('--max-nsec', help='Histogram max value in nanoseconds.', type=int)
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
                   type=int)
    p.add_argument('--rw-mbytes-per-sec',
                   help="R/W megabytes per second limit (>=1, example: 100). 0 means unlimited.",
                   type=int)
    p.add_argument('--r-mbytes-per-sec',
                   help="Read megabytes per second limit (>=1, example: 100). 0 means unlimited.",
                   type=int)
    p.add_argument('--w-mbytes-per-sec',
                   help="Write megabytes per second limit (>=1, example: 100). 0 means unlimited.",
                   type=int)
    p.set_defaults(func=bdev_set_qos_limit)

    def bdev_error_inject_error(args):
        rpc.bdev.bdev_error_inject_error(args.client,
                                         name=args.name,
                                         io_type=args.io_type,
                                         error_type=args.error_type,
                                         num=args.num,
                                         queue_depth=args.queue_depth,
                                         corrupt_offset=args.corrupt_offset,
                                         corrupt_value=args.corrupt_value)

    p = subparsers.add_parser('bdev_error_inject_error', help='bdev inject error')
    p.add_argument('name', help="""the name of the error injection bdev""")
    p.add_argument('io_type', help="""io_type: 'clear' 'read' 'write' 'unmap' 'flush' 'all'""")
    p.add_argument('error_type', help="""error_type: 'failure' 'pending' 'corrupt_data' 'nomem'""")
    p.add_argument(
        '-n', '--num', help='the number of commands you want to fail', type=int)
    p.add_argument(
        '-q', '--queue-depth', help='the queue depth at which to trigger the error', type=int)
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
