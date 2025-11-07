#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2016 Intel Corporation
#  All rights reserved.
#  Copyright (c) 2022 Dell Inc, or its subsidiaries.
#  Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
#

import argparse

from spdk.rpc.client import print_array, print_dict, print_json  # noqa  # noqa
from spdk.rpc.cmd_parser import strip_globals
from spdk.rpc.helpers import DeprecateFalseAction, DeprecateTrueAction


def add_parser(subparsers):

    def bdev_nvme_start_mdns_discovery(args):
        args.client.bdev_nvme_start_mdns_discovery(
                                                name=args.name,
                                                svcname=args.svcname,
                                                hostnqn=args.hostnqn)

    p = subparsers.add_parser('bdev_nvme_start_mdns_discovery', help='Start mdns based automatic discovery')
    p.add_argument('-b', '--name', help="Name of the NVMe controller prefix for each bdev name", required=True)
    p.add_argument('-s', '--svcname', help='Service type to discover: e.g., _nvme-disc._tcp', required=True)
    p.add_argument('-q', '--hostnqn', help='NVMe-oF host subnqn')
    p.set_defaults(func=bdev_nvme_start_mdns_discovery)

    def bdev_nvme_stop_mdns_discovery(args):
        args.client.bdev_nvme_stop_mdns_discovery(name=args.name)

    p = subparsers.add_parser('bdev_nvme_stop_mdns_discovery', help='Stop automatic mdns discovery')
    p.add_argument('-b', '--name', help="Name of the service to stop", required=True)
    p.set_defaults(func=bdev_nvme_stop_mdns_discovery)

    def bdev_nvme_get_mdns_discovery_info(args):
        print_dict(args.client.bdev_nvme_get_mdns_discovery_info())

    p = subparsers.add_parser('bdev_nvme_get_mdns_discovery_info', help='Get information about the automatic mdns discovery')
    p.set_defaults(func=bdev_nvme_get_mdns_discovery_info)

    def bdev_set_options(args):
        args.client.bdev_set_options(
                                  bdev_io_pool_size=args.bdev_io_pool_size,
                                  bdev_io_cache_size=args.bdev_io_cache_size,
                                  bdev_auto_examine=args.bdev_auto_examine,
                                  iobuf_small_cache_size=args.iobuf_small_cache_size,
                                  iobuf_large_cache_size=args.iobuf_large_cache_size)

    p = subparsers.add_parser('bdev_set_options',
                              help="""Set options of bdev subsystem""")
    p.add_argument('-p', '--bdev-io-pool-size', help='Number of bdev_io structures in shared buffer pool', type=int)
    p.add_argument('-c', '--bdev-io-cache-size', help='Maximum number of bdev_io structures cached per thread', type=int)
    # TODO: this group is deprecated, remove in next version
    group = p.add_mutually_exclusive_group()
    group.add_argument('-e', '--enable-auto-examine', dest='bdev_auto_examine', action=DeprecateTrueAction,
                       help='Allow to auto examine', default=True)
    group.add_argument('-d', '--disable-auto-examine', dest='bdev_auto_examine', action=DeprecateFalseAction,
                       help='Not allow to auto examine')
    group.add_argument('--auto-examine', dest='bdev_auto_examine', action=argparse.BooleanOptionalAction,
                       help='Enable or disable auto examine')
    p.add_argument('--iobuf-small-cache-size', help='Size of the small iobuf per thread cache', type=int)
    p.add_argument('--iobuf-large-cache-size', help='Size of the large iobuf per thread cache', type=int)
    p.set_defaults(func=bdev_set_options)

    def bdev_examine(args):
        args.client.bdev_examine(name=args.name)

    p = subparsers.add_parser('bdev_examine',
                              help="""examine a bdev if it exists, or will examine it after it is created""")
    p.add_argument('-b', '--name', help='Name or alias of the bdev', required=True)
    p.set_defaults(func=bdev_examine)

    def bdev_wait_for_examine(args):
        args.client.bdev_wait_for_examine()

    p = subparsers.add_parser('bdev_wait_for_examine',
                              help="""Report when all bdevs have been examined""")
    p.set_defaults(func=bdev_wait_for_examine)

    def bdev_crypto_create(args):
        print_json(args.client.bdev_crypto_create(
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
        args.client.bdev_crypto_delete(name=args.name)

    p = subparsers.add_parser('bdev_crypto_delete', help='Delete a crypto disk')
    p.add_argument('name', help='crypto bdev name')
    p.set_defaults(func=bdev_crypto_delete)

    def bdev_ocf_create(args):
        print_json(args.client.bdev_ocf_create(
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
        args.client.bdev_ocf_delete(name=args.name)

    p = subparsers.add_parser('bdev_ocf_delete', help='Delete an OCF block device')
    p.add_argument('name', help='Name of OCF bdev')
    p.set_defaults(func=bdev_ocf_delete)

    def bdev_ocf_get_stats(args):
        print_dict(args.client.bdev_ocf_get_stats(name=args.name))
    p = subparsers.add_parser('bdev_ocf_get_stats', help='Get statistics of chosen OCF block device')
    p.add_argument('name', help='Name of OCF bdev')
    p.set_defaults(func=bdev_ocf_get_stats)

    def bdev_ocf_reset_stats(args):
        print_dict(args.client.bdev_ocf_reset_stats(name=args.name))
    p = subparsers.add_parser('bdev_ocf_reset_stats', help='Reset statistics of chosen OCF block device')
    p.add_argument('name', help='Name of OCF bdev')
    p.set_defaults(func=bdev_ocf_reset_stats)

    def bdev_ocf_get_bdevs(args):
        print_dict(args.client.bdev_ocf_get_bdevs(name=args.name))
    p = subparsers.add_parser('bdev_ocf_get_bdevs', help='Get list of OCF devices including unregistered ones')
    p.add_argument('name', nargs='?', help='name of OCF vbdev or name of cache device or name of core device (optional)')
    p.set_defaults(func=bdev_ocf_get_bdevs)

    def bdev_ocf_set_cache_mode(args):
        print_json(args.client.bdev_ocf_set_cache_mode(
                                                    name=args.name,
                                                    mode=args.mode))
    p = subparsers.add_parser('bdev_ocf_set_cache_mode',
                              help='Set cache mode of OCF block device')
    p.add_argument('name', help='Name of OCF bdev')
    p.add_argument('mode', help='OCF cache mode', choices=['wb', 'wt', 'pt', 'wa', 'wi', 'wo'])
    p.set_defaults(func=bdev_ocf_set_cache_mode)

    def bdev_ocf_set_seqcutoff(args):
        args.client.bdev_ocf_set_seqcutoff(
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
        args.client.bdev_ocf_flush_start(name=args.name)
    p = subparsers.add_parser('bdev_ocf_flush_start',
                              help='Start flushing OCF cache device')
    p.add_argument('name', help='Name of OCF bdev')
    p.set_defaults(func=bdev_ocf_flush_start)

    def bdev_ocf_flush_status(args):
        print_json(args.client.bdev_ocf_flush_status(name=args.name))
    p = subparsers.add_parser('bdev_ocf_flush_status',
                              help='Get flush status of OCF cache device')
    p.add_argument('name', help='Name of OCF bdev')
    p.set_defaults(func=bdev_ocf_flush_status)

    def bdev_malloc_create(args):
        num_blocks = (args.total_size * 1024 * 1024) // args.block_size
        print_json(args.client.bdev_malloc_create(
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
        args.client.bdev_malloc_delete(name=args.name)

    p = subparsers.add_parser('bdev_malloc_delete', help='Delete a malloc disk')
    p.add_argument('name', help='malloc bdev name')
    p.set_defaults(func=bdev_malloc_delete)

    def bdev_null_create(args):
        num_blocks = (args.total_size * 1024 * 1024) // args.block_size
        if args.dif_type and not args.md_size:
            print("ERROR: --md-size must be > 0 when --dif-type is > 0")
            exit(1)
        print_json(args.client.bdev_null_create(
                                             num_blocks=num_blocks,
                                             block_size=args.block_size,
                                             physical_block_size=args.physical_block_size,
                                             name=args.name,
                                             uuid=args.uuid,
                                             md_size=args.md_size,
                                             dif_type=args.dif_type,
                                             dif_is_head_of_md=args.dif_is_head_of_md,
                                             dif_pi_format=args.dif_pi_format,
                                             preferred_write_alignment=args.preferred_write_alignment,
                                             preferred_write_granularity=args.preferred_write_granularity,
                                             optimal_write_size=args.optimal_write_size,
                                             preferred_unmap_alignment=args.preferred_unmap_alignment,
                                             preferred_unmap_granularity=args.preferred_unmap_granularity))

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
    p.add_argument('-a', '--preferred-write-alignment', help='Preferred write alignment in blocks.', type=int)
    p.add_argument('-g', '--preferred-write-granularity', help='Preferred write granularity in blocks.', type=int)
    p.add_argument('-s', '--optimal-write-size', help='Optimal write size in blocks.', type=int)
    p.add_argument('-c', '--preferred-unmap-alignment', help='Preferred unmap granularity in blocks.', type=int)
    p.add_argument('-r', '--preferred-unmap-granularity', help='Preferred unmap granularity in blocks.', type=int)
    p.set_defaults(func=bdev_null_create)

    def bdev_null_delete(args):
        args.client.bdev_null_delete(name=args.name)

    p = subparsers.add_parser('bdev_null_delete', help='Delete a null bdev')
    p.add_argument('name', help='null bdev name')
    p.set_defaults(func=bdev_null_delete)

    def bdev_null_resize(args):
        print_json(args.client.bdev_null_resize(
                                             name=args.name,
                                             new_size=args.new_size))

    p = subparsers.add_parser('bdev_null_resize',
                              help='Resize a null bdev')
    p.add_argument('name', help='null bdev name')
    p.add_argument('new_size', help='new bdev size for resize operation. The unit is MiB', type=int)
    p.set_defaults(func=bdev_null_resize)

    def bdev_aio_create(args):
        print_json(args.client.bdev_aio_create(
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
        print_json(args.client.bdev_aio_rescan(name=args.name))

    p = subparsers.add_parser('bdev_aio_rescan', help='Rescan a bdev size with aio backend')
    p.add_argument('name', help='Block device name')
    p.set_defaults(func=bdev_aio_rescan)

    def bdev_aio_delete(args):
        args.client.bdev_aio_delete(name=args.name)

    p = subparsers.add_parser('bdev_aio_delete', help='Delete an aio disk')
    p.add_argument('name', help='aio bdev name')
    p.set_defaults(func=bdev_aio_delete)

    def bdev_uring_create(args):
        print_json(args.client.bdev_uring_create(
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
        print_json(args.client.bdev_uring_rescan(name=args.name))

    p = subparsers.add_parser('bdev_uring_rescan', help='Rescan a bdev size with uring backend')
    p.add_argument('name', help='Block device name')
    p.set_defaults(func=bdev_uring_rescan)

    def bdev_uring_delete(args):
        args.client.bdev_uring_delete(name=args.name)

    p = subparsers.add_parser('bdev_uring_delete', help='Delete a uring bdev')
    p.add_argument('name', help='uring bdev name')
    p.set_defaults(func=bdev_uring_delete)

    def bdev_xnvme_create(args):
        print_json(args.client.bdev_xnvme_create(
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
        args.client.bdev_xnvme_delete(name=args.name)

    p = subparsers.add_parser('bdev_xnvme_delete', help='Delete a xNVMe bdev')
    p.add_argument('name', help='xNVMe bdev name')
    p.set_defaults(func=bdev_xnvme_delete)

    def bdev_nvme_set_options(args):
        params = strip_globals(vars(args))
        args.client.bdev_nvme_set_options(**params)

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
    # TODO: this group is deprecated, remove in next version
    group = p.add_mutually_exclusive_group()
    group.add_argument('--enable-rdma-umr-per-io',
                       help='''Enable scatter-gather RDMA Memory Region per IO if supported by the system.''',
                       action=DeprecateTrueAction, dest='rdma_umr_per_io')
    group.add_argument('--disable-rdma-umr-per-io',
                       help='''Disable scatter-gather RDMA Memory Region per IO.''',
                       action=DeprecateFalseAction, dest='rdma_umr_per_io')
    group.add_argument('--rdma-umr-per-io', action=argparse.BooleanOptionalAction,
                       help='Enable or disable scatter-gather RDMA Memory Region per IO if supported by the system.')
    p.add_argument('--tcp-connect-timeout-ms',
                   help='Time to wait until TCP connection is done. Default: 0 (no timeout).', type=int)
    p.add_argument('--enable-flush', help='Pass flush to NVMe when volatile write cache is present',
                   action='store_true')

    p.set_defaults(func=bdev_nvme_set_options)

    def bdev_nvme_set_hotplug(args):
        args.client.bdev_nvme_set_hotplug(enable=args.enable, period_us=args.period_us)

    p = subparsers.add_parser('bdev_nvme_set_hotplug', help='Set hotplug options for bdev nvme type.')
    # TODO: this group is deprecated, remove in next version
    group = p.add_mutually_exclusive_group(required=True)
    group.add_argument('-d', '--disable', dest='enable', action=DeprecateFalseAction, help="Disable hotplug (default)", default=False)
    group.add_argument('-e', '--enable', dest='enable', action=DeprecateTrueAction, help="Enable hotplug")
    group.add_argument('--hotplug', dest='enable', action=argparse.BooleanOptionalAction, help='Enable or disable hotplug')
    p.add_argument('-r', '--period-us',
                   help='How often the hotplug is processed for insert and remove events', type=int)
    p.set_defaults(func=bdev_nvme_set_hotplug)

    def bdev_nvme_attach_controller(args):
        print_array(args.client.bdev_nvme_attach_controller(
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
        print_dict(args.client.bdev_nvme_get_controllers(name=args.name))

    p = subparsers.add_parser(
        'bdev_nvme_get_controllers', help='Display current NVMe controllers list or required NVMe controller')
    p.add_argument('-n', '--name', help="Name of the NVMe controller. Example: Nvme0")
    p.set_defaults(func=bdev_nvme_get_controllers)

    def bdev_nvme_detach_controller(args):
        args.client.bdev_nvme_detach_controller(
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
        args.client.bdev_nvme_reset_controller(
                                            name=args.name,
                                            cntlid=args.cntlid)

    p = subparsers.add_parser('bdev_nvme_reset_controller',
                              help='Reset an NVMe controller or all NVMe controllers in an NVMe bdev controller')
    p.add_argument('name', help="Name of the NVMe controller")
    p.add_argument('-c', '--cntlid', help="NVMe controller ID", type=int)
    p.set_defaults(func=bdev_nvme_reset_controller)

    def bdev_nvme_enable_controller(args):
        args.client.bdev_nvme_enable_controller(
                                             name=args.name,
                                             cntlid=args.cntlid)

    p = subparsers.add_parser('bdev_nvme_enable_controller',
                              help='Enable an NVMe controller or all NVMe controllers in an NVMe bdev controller')
    p.add_argument('name', help="Name of the NVMe controller")
    p.add_argument('-c', '--cntlid', help="NVMe controller ID", type=int)
    p.set_defaults(func=bdev_nvme_enable_controller)

    def bdev_nvme_disable_controller(args):
        args.client.bdev_nvme_disable_controller(
                                              name=args.name,
                                              cntlid=args.cntlid)

    p = subparsers.add_parser('bdev_nvme_disable_controller',
                              help='Disable an NVMe controller or all NVMe controllers in an NVMe bdev controller')
    p.add_argument('name', help="Name of the NVMe controller")
    p.add_argument('-c', '--cntlid', help="NVMe controller ID", type=int)
    p.set_defaults(func=bdev_nvme_disable_controller)

    def bdev_nvme_start_discovery(args):
        args.client.bdev_nvme_start_discovery(
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
        args.client.bdev_nvme_stop_discovery(name=args.name)

    p = subparsers.add_parser('bdev_nvme_stop_discovery', help='Stop automatic discovery')
    p.add_argument('-b', '--name', help="Name of the service to stop", required=True)
    p.set_defaults(func=bdev_nvme_stop_discovery)

    def bdev_nvme_get_discovery_info(args):
        print_dict(args.client.bdev_nvme_get_discovery_info())

    p = subparsers.add_parser('bdev_nvme_get_discovery_info', help='Get information about the automatic discovery')
    p.set_defaults(func=bdev_nvme_get_discovery_info)

    def bdev_nvme_get_io_paths(args):
        print_dict(args.client.bdev_nvme_get_io_paths(name=args.name))

    p = subparsers.add_parser('bdev_nvme_get_io_paths', help='Display active I/O paths')
    p.add_argument('-n', '--name', help="Name of the NVMe bdev")
    p.set_defaults(func=bdev_nvme_get_io_paths)

    def bdev_nvme_set_preferred_path(args):
        args.client.bdev_nvme_set_preferred_path(
                                              name=args.name,
                                              cntlid=args.cntlid)

    p = subparsers.add_parser('bdev_nvme_set_preferred_path',
                              help="""Set the preferred I/O path for an NVMe bdev when in multipath mode""")
    p.add_argument('-b', '--name', help='Name of the NVMe bdev', required=True)
    p.add_argument('-c', '--cntlid', help='NVMe-oF controller ID', type=int, required=True)
    p.set_defaults(func=bdev_nvme_set_preferred_path)

    def bdev_nvme_set_multipath_policy(args):
        args.client.bdev_nvme_set_multipath_policy(
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
        print_dict(args.client.bdev_nvme_get_path_iostat(name=args.name))

    p = subparsers.add_parser('bdev_nvme_get_path_iostat',
                              help="""Display current I/O statistics of all the IO paths of the blockdev. It can be
                              called when io_path_stat is true.""")
    p.add_argument('-b', '--name', help="Name of the Blockdev. Example: NVMe0n1", required=True)
    p.set_defaults(func=bdev_nvme_get_path_iostat)

    def bdev_nvme_cuse_register(args):
        args.client.bdev_nvme_cuse_register(name=args.name)

    p = subparsers.add_parser('bdev_nvme_cuse_register',
                              help='Register CUSE devices on NVMe controller')
    p.add_argument('-n', '--name',
                   help='Name of the NVMe controller. Example: Nvme0', required=True)
    p.set_defaults(func=bdev_nvme_cuse_register)

    def bdev_nvme_cuse_unregister(args):
        args.client.bdev_nvme_cuse_unregister(name=args.name)

    p = subparsers.add_parser('bdev_nvme_cuse_unregister',
                              help='Unregister CUSE devices on NVMe controller')
    p.add_argument('-n', '--name',
                   help='Name of the NVMe controller. Example: Nvme0', required=True)
    p.set_defaults(func=bdev_nvme_cuse_unregister)

    def bdev_nvme_set_keys(args):
        args.client.bdev_nvme_set_keys(name=args.name,
                                       dhchap_key=args.dhchap_key,
                                       dhchap_ctrlr_key=args.dhchap_ctrlr_key)

    p = subparsers.add_parser('bdev_nvme_set_keys',
                              help='Set DH-HMAC-CHAP keys and force (re)authentication on all '
                              'connected qpairs')
    p.add_argument('name', help='Name of the NVMe controller')
    p.add_argument('--dhchap-key', help='DH-HMAC-CHAP key name')
    p.add_argument('--dhchap-ctrlr-key', help='DH-HMAC-CHAP controller key name')
    p.set_defaults(func=bdev_nvme_set_keys)

    def bdev_zone_block_create(args):
        print_json(args.client.bdev_zone_block_create(
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
        args.client.bdev_zone_block_delete(name=args.name)

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
        print_json(args.client.bdev_rbd_register_cluster(
                                                      name=args.name,
                                                      user_id=args.user_id,
                                                      config_param=config_param,
                                                      config_file=args.config_file,
                                                      key_file=args.key_file,
                                                      core_mask=args.core_mask))

    p = subparsers.add_parser('bdev_rbd_register_cluster',
                              help='Add a Rados cluster with ceph rbd backend')
    p.add_argument('name', help="Name of the Rados cluster only known to rbd bdev")
    p.add_argument('--user', dest='user_id', help="Ceph user name (i.e. admin, not client.admin)")
    p.add_argument('--config-param', action='append', metavar='key=value',
                   help="adds a key=value configuration option for rados_conf_set (default: rely on config file)")
    p.add_argument('--config-file', help="The file path of the Rados configuration file")
    p.add_argument('--key-file', help="The file path of the Rados keyring file")
    p.add_argument('--core-mask', help="Set core mask for librbd IO context threads")
    p.set_defaults(func=bdev_rbd_register_cluster)

    def bdev_rbd_unregister_cluster(args):
        args.client.bdev_rbd_unregister_cluster(name=args.name)

    p = subparsers.add_parser('bdev_rbd_unregister_cluster',
                              help='Unregister a Rados cluster object')
    p.add_argument('name', help='Name of the Rados Cluster only known to rbd bdev')
    p.set_defaults(func=bdev_rbd_unregister_cluster)

    def bdev_rbd_get_clusters_info(args):
        print_json(args.client.bdev_rbd_get_clusters_info(name=args.name))

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
        print_json(args.client.bdev_rbd_create(
                                            name=args.name,
                                            user_id=args.user_id,
                                            config=config,
                                            pool_name=args.pool_name,
                                            rbd_name=args.rbd_name,
                                            block_size=args.block_size,
                                            cluster_name=args.cluster_name,
                                            uuid=args.uuid,
                                            read_only=args.read_only))

    p = subparsers.add_parser('bdev_rbd_create', help='Add a bdev with ceph rbd backend')
    p.add_argument('-b', '--name', help="Name of the bdev")
    p.add_argument('--user', dest='user_id', help="Ceph user name (i.e. admin, not client.admin)")
    p.add_argument('--config', action='append', metavar='key=value',
                   help="adds a key=value configuration option for rados_conf_set (default: rely on config file)")
    p.add_argument('pool_name', help='rbd pool name')
    p.add_argument('rbd_name', help='rbd image name')
    p.add_argument('block_size', help='rbd block size', type=int)
    p.add_argument('-c', '--cluster-name', help="cluster name to identify the Rados cluster")
    p.add_argument('-u', '--uuid', help="UUID of the bdev")
    p.add_argument('-r', '--readonly', dest='read_only', action='store_true', help='Set this bdev as read-only')
    p.set_defaults(func=bdev_rbd_create)

    def bdev_rbd_delete(args):
        args.client.bdev_rbd_delete(name=args.name)

    p = subparsers.add_parser('bdev_rbd_delete', help='Delete a rbd bdev')
    p.add_argument('name', help='rbd bdev name')
    p.set_defaults(func=bdev_rbd_delete)

    def bdev_rbd_resize(args):
        print_json(args.client.bdev_rbd_resize(
                                            name=args.name,
                                            new_size=args.new_size))

    p = subparsers.add_parser('bdev_rbd_resize',
                              help='Resize a rbd bdev')
    p.add_argument('name', help='rbd bdev name')
    p.add_argument('new_size', help='new bdev size for resize operation. The unit is MiB', type=int)
    p.set_defaults(func=bdev_rbd_resize)

    def bdev_delay_create(args):
        print_json(args.client.bdev_delay_create(
                                              base_bdev_name=args.base_bdev_name,
                                              name=args.name,
                                              uuid=args.uuid,
                                              avg_read_latency=args.avg_read_latency,
                                              p99_read_latency=args.p99_read_latency,
                                              avg_write_latency=args.avg_write_latency,
                                              p99_write_latency=args.p99_write_latency))

    p = subparsers.add_parser('bdev_delay_create',
                              help='Add a delay bdev on existing bdev')
    p.add_argument('-b', '--base-bdev-name', help="Name of the existing bdev", required=True)
    p.add_argument('-d', '--name', help="Name of the delay bdev", required=True)
    p.add_argument('-u', '--uuid', help='UUID of the bdev (optional)')
    p.add_argument('-r', '--avg-read-latency',
                   help="Average latency to apply before completing read ops (in microseconds)", required=True, type=int)
    p.add_argument('-t', '--nine-nine-read-latency', dest='p99_read_latency',
                   help="latency to apply to 1 in 100 read ops (in microseconds)", required=True, type=int)
    p.add_argument('-w', '--avg-write-latency',
                   help="Average latency to apply before completing write ops (in microseconds)", required=True, type=int)
    p.add_argument('-n', '--nine-nine-write-latency', dest='p99_write_latency',
                   help="latency to apply to 1 in 100 write ops (in microseconds)", required=True, type=int)
    p.set_defaults(func=bdev_delay_create)

    def bdev_delay_delete(args):
        args.client.bdev_delay_delete(name=args.name)

    p = subparsers.add_parser('bdev_delay_delete', help='Delete a delay bdev')
    p.add_argument('name', help='delay bdev name')
    p.set_defaults(func=bdev_delay_delete)

    def bdev_delay_update_latency(args):
        print_json(args.client.bdev_delay_update_latency(
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
        print_json(args.client.bdev_error_create(
                                              base_name=args.base_name,
                                              uuid=args.uuid))

    p = subparsers.add_parser('bdev_error_create', help='Add bdev with error injection backend')
    p.add_argument('base_name', help='base bdev name')
    p.add_argument('--uuid', help='UUID for this bdev')
    p.set_defaults(func=bdev_error_create)

    def bdev_error_delete(args):
        args.client.bdev_error_delete(name=args.name)

    p = subparsers.add_parser('bdev_error_delete', help='Delete an error bdev')
    p.add_argument('name', help='error bdev name')
    p.set_defaults(func=bdev_error_delete)

    def bdev_iscsi_set_options(args):
        args.client.bdev_iscsi_set_options(timeout_sec=args.timeout_sec)

    p = subparsers.add_parser('bdev_iscsi_set_options', help='Set options for the bdev iscsi type.')
    p.add_argument('-t', '--timeout-sec', help="Timeout for command, in seconds, if 0, don't track timeout.", type=int)
    p.set_defaults(func=bdev_iscsi_set_options)

    def bdev_iscsi_create(args):
        print_json(args.client.bdev_iscsi_create(
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
        args.client.bdev_iscsi_delete(name=args.name)

    p = subparsers.add_parser('bdev_iscsi_delete', help='Delete an iSCSI bdev')
    p.add_argument('name', help='iSCSI bdev name')
    p.set_defaults(func=bdev_iscsi_delete)

    def bdev_passthru_create(args):
        print_json(args.client.bdev_passthru_create(
                                                 base_bdev_name=args.base_bdev_name,
                                                 name=args.name,
                                                 uuid=args.uuid))

    p = subparsers.add_parser('bdev_passthru_create', help='Add a pass through bdev on existing bdev')
    p.add_argument('-b', '--base-bdev-name', help="Name of the existing bdev", required=True)
    p.add_argument('-p', '--name', help="Name of the pass through bdev", required=True)
    p.add_argument('-u', '--uuid', help="UUID of the bdev")
    p.set_defaults(func=bdev_passthru_create)

    def bdev_passthru_delete(args):
        args.client.bdev_passthru_delete(name=args.name)

    p = subparsers.add_parser('bdev_passthru_delete', help='Delete a pass through bdev')
    p.add_argument('name', help='pass through bdev name')
    p.set_defaults(func=bdev_passthru_delete)

    def bdev_get_bdevs(args):
        print_dict(args.client.bdev_get_bdevs(name=args.name, timeout=args.timeout))

    p = subparsers.add_parser('bdev_get_bdevs',
                              help='Display current blockdev list or required blockdev')
    p.add_argument('-b', '--name', help="Name of the Blockdev. Example: Nvme0n1")
    p.add_argument('-t', '--timeout-ms', dest='timeout', help="""Time in ms to wait for the bdev to appear (only used
    with the -b|--name option). The default timeout is 0, meaning the RPC returns immediately
    whether the bdev exists or not.""",
                   type=int)
    p.set_defaults(func=bdev_get_bdevs)

    def bdev_get_iostat(args):
        names = None
        if args.names:
            names = []
            for i in args.names.strip().split(','):
                names.append(i)
        print_dict(args.client.bdev_get_iostat(
                                            name=args.name,
                                            per_channel=args.per_channel,
                                            reset_mode=args.reset_mode,
                                            names=names))

    p = subparsers.add_parser('bdev_get_iostat',
                              help='Display current I/O statistics of all the blockdevs or specified blockdev.')
    p.add_argument('-c', '--per-channel', default=False, dest='per_channel', help='Display per channel IO stats for specified device',
                   action='store_true')
    p.add_argument('--reset-mode', help="Mode to reset I/O statistics after getting", choices=['all', 'maxmin', 'none'])
    p.set_defaults(func=bdev_get_iostat)
    group = p.add_mutually_exclusive_group()
    group.add_argument('-b', '--name', help="Name of the Blockdev. Example: Nvme0n1")
    group.add_argument('--names', help='Bdev names to obtain I/O statistics from, comma-separated list in quotes')

    def bdev_reset_iostat(args):
        args.client.bdev_reset_iostat(name=args.name, mode=args.mode)

    p = subparsers.add_parser('bdev_reset_iostat',
                              help='Reset I/O statistics of all the blockdevs or specified blockdev.')
    p.add_argument('-b', '--name', help="Name of the Blockdev. Example: Nvme0n1")
    p.add_argument('-m', '--mode', help="Mode to reset I/O statistics", choices=['all', 'maxmin', 'none'])
    p.set_defaults(func=bdev_reset_iostat)

    def bdev_enable_histogram(args):
        args.client.bdev_enable_histogram(name=args.name, enable=args.enable, opc=args.opc,
                                          granularity=args.granularity, min_nsec=args.min_nsec, max_nsec=args.max_nsec)

    p = subparsers.add_parser('bdev_enable_histogram',
                              help='Enable or disable histogram for specified bdev')
    # TODO: this group is deprecated, remove in next version
    group = p.add_mutually_exclusive_group(required=True)
    group.add_argument('-e', '--enable',  dest='enable', action=DeprecateTrueAction,
                       help='Enable histograms on specified device', default=True)
    group.add_argument('-d', '--disable', dest='enable', action=DeprecateFalseAction,
                       help='Disable histograms on specified device')
    group.add_argument('--histogram', dest='enable', action=argparse.BooleanOptionalAction,
                       help='Enable or disable histograms on specified device')
    p.add_argument('-o', '--opc', help='Enable histogram for specified io type. Defaults to all io types if not specified.'
                   ' Refer to bdev_get_bdevs RPC for the list of io types.')
    p.add_argument('--granularity', help='Histogram bucket granularity.', type=int)
    p.add_argument('--min-nsec', help='Histogram min value in nanoseconds.', type=int)
    p.add_argument('--max-nsec', help='Histogram max value in nanoseconds.', type=int)
    p.add_argument('name', help='bdev name')
    p.set_defaults(func=bdev_enable_histogram)

    def bdev_get_histogram(args):
        print_dict(args.client.bdev_get_histogram(name=args.name))

    p = subparsers.add_parser('bdev_get_histogram',
                              help='Get histogram for specified bdev')
    p.add_argument('name', help='bdev name')
    p.set_defaults(func=bdev_get_histogram)

    def bdev_set_qd_sampling_period(args):
        args.client.bdev_set_qd_sampling_period(
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
        args.client.bdev_set_qos_limit(
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
        args.client.bdev_error_inject_error(
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
        print_dict(args.client.bdev_nvme_apply_firmware(
                                                     bdev_name=args.bdev_name,
                                                     filename=args.filename))

    p = subparsers.add_parser('bdev_nvme_apply_firmware', help='Download and commit firmware to NVMe device')
    p.add_argument('filename', help='filename of the firmware to download')
    p.add_argument('bdev_name', help='name of the NVMe device')
    p.set_defaults(func=bdev_nvme_apply_firmware)

    def bdev_nvme_get_transport_statistics(args):
        print_dict(args.client.bdev_nvme_get_transport_statistics())

    p = subparsers.add_parser('bdev_nvme_get_transport_statistics',
                              help='Get bdev_nvme poll group transport statistics')
    p.set_defaults(func=bdev_nvme_get_transport_statistics)

    def bdev_nvme_get_controller_health_info(args):
        print_dict(args.client.bdev_nvme_get_controller_health_info(name=args.name))

    p = subparsers.add_parser('bdev_nvme_get_controller_health_info',
                              help='Display health log of the required NVMe bdev controller.')
    p.add_argument('-c', '--name', help="Name of the NVMe bdev controller. Example: Nvme0", required=True)
    p.set_defaults(func=bdev_nvme_get_controller_health_info)

    # raid
    def bdev_raid_set_options(args):
        args.client.bdev_raid_set_options(
                                       process_window_size_kb=args.process_window_size_kb,
                                       process_max_bandwidth_mb_sec=args.process_max_bandwidth_mb_sec)

    p = subparsers.add_parser('bdev_raid_set_options',
                              help='Set options for bdev raid.')
    p.add_argument('-w', '--process-window-size-kb', type=int,
                   help="Background process (e.g. rebuild) window size in KiB")
    p.add_argument('-b', '--process-max-bandwidth-mb-sec', type=int,
                   help="Background process (e.g. rebuild) maximum bandwidth in MiB/Sec")

    p.set_defaults(func=bdev_raid_set_options)

    def bdev_raid_get_bdevs(args):
        print_json(args.client.bdev_raid_get_bdevs(
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
        for u in args.base_bdevs.strip().split():
            base_bdevs.append(u)

        args.client.bdev_raid_create(
                                  name=args.name,
                                  strip_size_kb=args.strip_size_kb,
                                  raid_level=args.raid_level,
                                  base_bdevs=base_bdevs,
                                  uuid=args.uuid,
                                  superblock=args.superblock)
    p = subparsers.add_parser('bdev_raid_create', help='Create new raid bdev')
    p.add_argument('-n', '--name', help='raid bdev name', required=True)
    p.add_argument('-z', '--strip-size-kb', help='strip size in KB', type=int)
    p.add_argument('-r', '--raid-level', help='raid level, raid0, raid1 and a special level concat are supported', required=True)
    p.add_argument('-b', '--base-bdevs', help='base bdevs name, whitespace separated list in quotes', required=True)
    p.add_argument('--uuid', help='UUID for this raid bdev')
    p.add_argument('-s', '--superblock', help='information about raid bdev will be stored in superblock on each base bdev, '
                                              'disabled by default due to backward compatibility', action='store_true')
    p.set_defaults(func=bdev_raid_create)

    def bdev_raid_delete(args):
        args.client.bdev_raid_delete(name=args.name)
    p = subparsers.add_parser('bdev_raid_delete', help='Delete existing raid bdev')
    p.add_argument('name', help='raid bdev name')
    p.set_defaults(func=bdev_raid_delete)

    def bdev_raid_add_base_bdev(args):
        args.client.bdev_raid_add_base_bdev(
                                         raid_bdev=args.raid_bdev,
                                         base_bdev=args.base_bdev)
    p = subparsers.add_parser('bdev_raid_add_base_bdev', help='Add base bdev to existing raid bdev')
    p.add_argument('raid_bdev', help='raid bdev name')
    p.add_argument('base_bdev', help='base bdev name')
    p.set_defaults(func=bdev_raid_add_base_bdev)

    def bdev_raid_remove_base_bdev(args):
        args.client.bdev_raid_remove_base_bdev(name=args.name)
    p = subparsers.add_parser('bdev_raid_remove_base_bdev', help='Remove base bdev from existing raid bdev')
    p.add_argument('name', help='base bdev name')
    p.set_defaults(func=bdev_raid_remove_base_bdev)

    # split
    def bdev_split_create(args):
        print_array(args.client.bdev_split_create(
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
        args.client.bdev_split_delete(base_bdev=args.base_bdev)

    p = subparsers.add_parser('bdev_split_delete', help="""Delete split config with all created splits.""")
    p.add_argument('base_bdev', help='base bdev name')
    p.set_defaults(func=bdev_split_delete)

    # ftl
    def bdev_ftl_create(args):
        print_dict(args.client.bdev_ftl_create(
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
        print_dict(args.client.bdev_ftl_load(
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
        print_dict(args.client.bdev_ftl_unload(name=args.name, fast_shutdown=args.fast_shutdown))

    p = subparsers.add_parser('bdev_ftl_unload', help='Unload FTL bdev')
    p.add_argument('-b', '--name', help="Name of the bdev", required=True)
    p.add_argument('-f', '--fast-shutdown', help="Fast shutdown", action='store_true')
    p.set_defaults(func=bdev_ftl_unload)

    def bdev_ftl_delete(args):
        print_dict(args.client.bdev_ftl_delete(name=args.name, fast_shutdown=args.fast_shutdown))

    p = subparsers.add_parser('bdev_ftl_delete', help='Delete FTL bdev')
    p.add_argument('-b', '--name', help="Name of the bdev", required=True)
    p.add_argument('-f', '--fast-shutdown', help="Fast shutdown", action='store_true')
    p.set_defaults(func=bdev_ftl_delete)

    def bdev_ftl_unmap(args):
        print_dict(args.client.bdev_ftl_unmap(name=args.name,
                                              lba=args.lba,
                                              num_blocks=args.num_blocks))

    p = subparsers.add_parser('bdev_ftl_unmap', help='FTL unmap')
    p.add_argument('-b', '--name', help="Name of the bdev", required=True)
    p.add_argument('--lba', help='start LBA', required=True, type=int)
    p.add_argument('--num-blocks', help='num blocks', required=True, type=int)
    p.set_defaults(func=bdev_ftl_unmap)

    def bdev_ftl_get_stats(args):
        print_dict(args.client.bdev_ftl_get_stats(name=args.name))

    p = subparsers.add_parser('bdev_ftl_get_stats', help='print ftl stats')
    p.add_argument('-b', '--name', help="Name of the bdev", required=True)
    p.set_defaults(func=bdev_ftl_get_stats)

    def bdev_ftl_get_properties(args):
        print_dict(args.client.bdev_ftl_get_properties(name=args.name))

    p = subparsers.add_parser('bdev_ftl_get_properties', help='Print FTL properties')
    p.add_argument('-b', '--name', help="Name of the bdev", required=True)
    p.set_defaults(func=bdev_ftl_get_properties)

    def bdev_ftl_set_property(args):
        print_dict(args.client.bdev_ftl_set_property(name=args.name,
                   ftl_property=args.property,
                   value=args.value))

    p = subparsers.add_parser('bdev_ftl_set_property', help='Set FTL property')
    p.add_argument('-b', '--name', help="Name of the bdev", required=True)
    p.add_argument('-p', '--property', help="Name of the property to be set", required=True)
    p.add_argument('-v', '--value', help="Value of the property", required=True)
    p.set_defaults(func=bdev_ftl_set_property)

    # opal
    def bdev_nvme_opal_init(args):
        args.client.bdev_nvme_opal_init(
                                     nvme_ctrlr_name=args.nvme_ctrlr_name,
                                     password=args.password)

    p = subparsers.add_parser('bdev_nvme_opal_init', help='take ownership and activate')
    p.add_argument('-b', '--nvme-ctrlr-name', help='nvme ctrlr name', required=True)
    p.add_argument('-p', '--password', help='password for admin', required=True)
    p.set_defaults(func=bdev_nvme_opal_init)

    def bdev_nvme_opal_revert(args):
        args.client.bdev_nvme_opal_revert(
                                       nvme_ctrlr_name=args.nvme_ctrlr_name,
                                       password=args.password)
    p = subparsers.add_parser('bdev_nvme_opal_revert', help='Revert to default factory settings')
    p.add_argument('-b', '--nvme-ctrlr-name', help='nvme ctrlr name', required=True)
    p.add_argument('-p', '--password', help='password', required=True)
    p.set_defaults(func=bdev_nvme_opal_revert)

    def bdev_opal_create(args):
        print_json(args.client.bdev_opal_create(
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
        print_dict(args.client.bdev_opal_get_info(
                                               bdev_name=args.bdev_name,
                                               password=args.password))

    p = subparsers.add_parser('bdev_opal_get_info', help='get opal locking range info for this bdev')
    p.add_argument('-b', '--bdev-name', help='opal bdev', required=True)
    p.add_argument('-p', '--password', help='password', required=True)
    p.set_defaults(func=bdev_opal_get_info)

    def bdev_opal_delete(args):
        args.client.bdev_opal_delete(
                                  bdev_name=args.bdev_name,
                                  password=args.password)

    p = subparsers.add_parser('bdev_opal_delete', help="""delete a virtual opal bdev""")
    p.add_argument('-b', '--bdev-name', help='opal virtual bdev', required=True)
    p.add_argument('-p', '--password', help='admin password', required=True)
    p.set_defaults(func=bdev_opal_delete)

    def bdev_opal_new_user(args):
        args.client.bdev_opal_new_user(
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
        args.client.bdev_opal_set_lock_state(
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
        print_dict(args.client.bdev_nvme_send_cmd(
                                               name=args.name,
                                               cmd_type=args.cmd_type,
                                               data_direction=args.data_direction,
                                               cmdbuf=args.cmdbuf,
                                               data=args.data,
                                               metadata=args.metadata,
                                               data_len=args.data_len,
                                               metadata_len=args.metadata_len,
                                               timeout_ms=args.timeout_ms))

    p = subparsers.add_parser('bdev_nvme_send_cmd', help='NVMe passthrough cmd.')
    p.add_argument('-n', '--nvme-name', dest='name', help="""Name of the operating NVMe controller""", required=True)
    p.add_argument('-t', '--cmd-type', help="""Type of nvme cmd. Valid values are: admin, io""", required=True)
    p.add_argument('-r', '--data-direction', help="""Direction of data transfer. Valid values are: c2h, h2c""", required=True)
    p.add_argument('-c', '--cmdbuf', help="""NVMe command encoded by base64 urlsafe""", required=True)
    p.add_argument('-d', '--data', help="""Data transferring to controller from host, encoded by base64 urlsafe""")
    p.add_argument('-m', '--metadata', help="""Metadata transferring to controller from host, encoded by base64 urlsafe""")
    p.add_argument('-D', '--data-length', dest='data_len', help="""Data length required to transfer from controller to host""", type=int)
    p.add_argument('-M', '--metadata-length', dest='metadata_len',
                   help="""Metadata length required to transfer from controller to host""", type=int)
    p.add_argument('-T', '--timeout-ms',
                   help="""Command execution timeout value, in milliseconds,  if 0, don't track timeout""", type=int)
    p.set_defaults(func=bdev_nvme_send_cmd)

    # bdev_nvme_add_error_injection
    def bdev_nvme_add_error_injection(args):
        print_dict(args.client.bdev_nvme_add_error_injection(
                                                          name=args.name,
                                                          cmd_type=args.cmd_type,
                                                          opc=args.opc,
                                                          do_not_submit=args.do_not_submit,
                                                          timeout_in_us=args.timeout_in_us,
                                                          err_count=args.err_count,
                                                          sct=args.sct,
                                                          sc=args.sc))
    p = subparsers.add_parser('bdev_nvme_add_error_injection',
                              help='Add a NVMe command error injection.')
    p.add_argument('-n', '--nvme-name', dest='name', help="""Name of the operating NVMe controller""", required=True)
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
        print_dict(args.client.bdev_nvme_remove_error_injection(
                                                             name=args.name,
                                                             cmd_type=args.cmd_type,
                                                             opc=args.opc))
    p = subparsers.add_parser('bdev_nvme_remove_error_injection',
                              help='Removes a NVMe command error injection.')
    p.add_argument('-n', '--nvme-name', dest='name', help="""Name of the operating NVMe controller""", required=True)
    p.add_argument('-t', '--cmd-type', help="""Type of nvme cmd. Valid values are: admin, io""", required=True)
    p.add_argument('-o', '--opc', help="""Opcode of the nvme cmd.""", required=True, type=int)
    p.set_defaults(func=bdev_nvme_remove_error_injection)

    # daos
    def bdev_daos_create(args):
        num_blocks = (args.total_size * 1024 * 1024) // args.block_size
        print_json(args.client.bdev_daos_create(
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
        args.client.bdev_daos_delete(name=args.name)

    p = subparsers.add_parser('bdev_daos_delete',
                              help='Delete a DAOS disk')
    p.add_argument('name', help='DAOS bdev name')
    p.set_defaults(func=bdev_daos_delete)

    def bdev_daos_resize(args):
        print_json(args.client.bdev_daos_resize(
                                             name=args.name,
                                             new_size=args.new_size))

    p = subparsers.add_parser('bdev_daos_resize',
                              help='Resize a DAOS bdev')
    p.add_argument('name', help='DAOS bdev name')
    p.add_argument('new_size', help='new bdev size for resize operation. The unit is MiB', type=int)
    p.set_defaults(func=bdev_daos_resize)
