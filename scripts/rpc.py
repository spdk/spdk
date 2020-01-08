#!/usr/bin/env python3

from rpc.client import print_dict, print_json, JSONRPCException
from rpc.helpers import deprecated_aliases

import logging
import argparse
import rpc
import sys
import shlex
import json

try:
    from shlex import quote
except ImportError:
    from pipes import quote


def print_array(a):
    print(" ".join((quote(v) for v in a)))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description='SPDK RPC command line interface')
    parser.add_argument('-s', dest='server_addr',
                        help='RPC domain socket path or IP address', default='/var/tmp/spdk.sock')
    parser.add_argument('-p', dest='port',
                        help='RPC port number (if server_addr is IP address)',
                        default=5260, type=int)
    parser.add_argument('-t', dest='timeout',
                        help='Timeout as a floating point number expressed in seconds waiting for response. Default: 60.0',
                        default=60.0, type=float)
    parser.add_argument('-v', dest='verbose', action='store_const', const="INFO",
                        help='Set verbose mode to INFO', default="ERROR")
    parser.add_argument('--verbose', dest='verbose', choices=['DEBUG', 'INFO', 'ERROR'],
                        help="""Set verbose level. """)
    parser.add_argument('--dry_run', dest='dry_run', action='store_true', help="Display request and exit")
    parser.set_defaults(dry_run=False)
    subparsers = parser.add_subparsers(help='RPC methods', dest='called_rpc_name')

    def framework_start_init(args):
        rpc.framework_start_init(args.client)

    p = subparsers.add_parser('framework_start_init', aliases=['start_subsystem_init'],
                              help='Start initialization of subsystems')
    p.set_defaults(func=framework_start_init)

    def framework_wait_init(args):
        rpc.framework_wait_init(args.client)

    p = subparsers.add_parser('framework_wait_init', aliases=['wait_subsystem_init'],
                              help='Block until subsystems have been initialized')
    p.set_defaults(func=framework_wait_init)

    def rpc_get_methods(args):
        print_dict(rpc.rpc_get_methods(args.client,
                                       current=args.current,
                                       include_aliases=args.include_aliases))

    p = subparsers.add_parser('rpc_get_methods', aliases=['get_rpc_methods'],
                              help='Get list of supported RPC methods')
    p.add_argument('-c', '--current', help='Get list of RPC methods only callable in the current state.', action='store_true')
    p.add_argument('-i', '--include-aliases', help='include RPC aliases', action='store_true')
    p.set_defaults(func=rpc_get_methods)

    def spdk_get_version(args):
        print_json(rpc.spdk_get_version(args.client))

    p = subparsers.add_parser('spdk_get_version', aliases=['get_spdk_version'],
                              help='Get SPDK version')
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
        rpc.load_config(args.client, sys.stdin,
                        include_aliases=args.include_aliases)

    p = subparsers.add_parser('load_config', help="""Configure SPDK subsystems and targets using JSON RPC read from stdin.""")
    p.add_argument('-i', '--include-aliases', help='include RPC aliases', action='store_true')
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
                                  sys.stdin)

    p = subparsers.add_parser('load_subsystem_config', help="""Configure SPDK subsystem using JSON RPC read from stdin.""")
    p.set_defaults(func=load_subsystem_config)

    # app
    def spdk_kill_instance(args):
        rpc.app.spdk_kill_instance(args.client,
                                   sig_name=args.sig_name)

    p = subparsers.add_parser('spdk_kill_instance', aliases=['kill_instance'],
                              help='Send signal to instance')
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

    p = subparsers.add_parser('framework_monitor_context_switch', aliases=['context_switch_monitor'],
                              help='Control whether the context switch monitor is enabled')
    p.add_argument('-e', '--enable', action='store_true', help='Enable context switch monitoring')
    p.add_argument('-d', '--disable', action='store_true', help='Disable context switch monitoring')
    p.set_defaults(func=framework_monitor_context_switch)

    def framework_get_reactors(args):
        print_dict(rpc.app.framework_get_reactors(args.client))

    p = subparsers.add_parser(
        'framework_get_reactors', help='Display list of all reactors')
    p.set_defaults(func=framework_get_reactors)

    # bdev
    def bdev_set_options(args):
        rpc.bdev.bdev_set_options(args.client,
                                  bdev_io_pool_size=args.bdev_io_pool_size,
                                  bdev_io_cache_size=args.bdev_io_cache_size)

    p = subparsers.add_parser('bdev_set_options', aliases=['set_bdev_options'],
                              help="""Set options of bdev subsystem""")
    p.add_argument('-p', '--bdev-io-pool-size', help='Number of bdev_io structures in shared buffer pool', type=int)
    p.add_argument('-c', '--bdev-io-cache-size', help='Maximum number of bdev_io structures cached per thread', type=int)
    p.set_defaults(func=bdev_set_options)

    def bdev_compress_create(args):
        print_json(rpc.bdev.bdev_compress_create(args.client,
                                                 base_bdev_name=args.base_bdev_name,
                                                 pm_path=args.pm_path))

    p = subparsers.add_parser('bdev_compress_create', aliases=['construct_compress_bdev'],
                              help='Add a compress vbdev')
    p.add_argument('-b', '--base_bdev_name', help="Name of the base bdev")
    p.add_argument('-p', '--pm_path', help="Path to persistent memory")
    p.set_defaults(func=bdev_compress_create)

    def bdev_compress_delete(args):
        rpc.bdev.bdev_compress_delete(args.client,
                                      name=args.name)

    p = subparsers.add_parser('bdev_compress_delete', aliases=['delete_compress_bdev'],
                              help='Delete a compress disk')
    p.add_argument('name', help='compress bdev name')
    p.set_defaults(func=bdev_compress_delete)

    def set_compress_pmd(args):
        rpc.bdev.set_compress_pmd(args.client,
                                  pmd=args.pmd)
    p = subparsers.add_parser('set_compress_pmd', help='Set pmd option for a compress disk')
    p.add_argument('-p', '--pmd', type=int, help='0 = auto-select, 1= QAT only, 2 = ISAL only')
    p.set_defaults(func=set_compress_pmd)

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
                                               key=args.key))
    p = subparsers.add_parser('bdev_crypto_create', aliases=['construct_crypto_bdev'],
                              help='Add a crypto vbdev')
    p.add_argument('base_bdev_name', help="Name of the base bdev")
    p.add_argument('name', help="Name of the crypto vbdev")
    p.add_argument('crypto_pmd', help="Name of the crypto device driver")
    p.add_argument('key', help="Key")
    p.set_defaults(func=bdev_crypto_create)

    def bdev_crypto_delete(args):
        rpc.bdev.bdev_crypto_delete(args.client,
                                    name=args.name)

    p = subparsers.add_parser('bdev_crypto_delete', aliases=['delete_crypto_bdev'],
                              help='Delete a crypto disk')
    p.add_argument('name', help='crypto bdev name')
    p.set_defaults(func=bdev_crypto_delete)

    def bdev_ocf_create(args):
        print_json(rpc.bdev.bdev_ocf_create(args.client,
                                            name=args.name,
                                            mode=args.mode,
                                            cache_bdev_name=args.cache_bdev_name,
                                            core_bdev_name=args.core_bdev_name))
    p = subparsers.add_parser('bdev_ocf_create', aliases=['construct_ocf_bdev'],
                              help='Add an OCF block device')
    p.add_argument('name', help='Name of resulting OCF bdev')
    p.add_argument('mode', help='OCF cache mode', choices=['wb', 'wt', 'pt', 'wa', 'wi', 'wo'])
    p.add_argument('cache_bdev_name', help='Name of underlying cache bdev')
    p.add_argument('core_bdev_name', help='Name of unerlying core bdev')
    p.set_defaults(func=bdev_ocf_create)

    def bdev_ocf_delete(args):
        rpc.bdev.bdev_ocf_delete(args.client,
                                 name=args.name)

    p = subparsers.add_parser('bdev_ocf_delete', aliases=['delete_ocf_bdev'],
                              help='Delete an OCF block device')
    p.add_argument('name', help='Name of OCF bdev')
    p.set_defaults(func=bdev_ocf_delete)

    def bdev_ocf_get_stats(args):
        print_dict(rpc.bdev.bdev_ocf_get_stats(args.client,
                                               name=args.name))
    p = subparsers.add_parser('bdev_ocf_get_stats', aliases=['get_ocf_stats'],
                              help='Get statistics of chosen OCF block device')
    p.add_argument('name', help='Name of OCF bdev')
    p.set_defaults(func=bdev_ocf_get_stats)

    def bdev_ocf_get_bdevs(args):
        print_dict(rpc.bdev.bdev_ocf_get_bdevs(args.client,
                                               name=args.name))
    p = subparsers.add_parser('bdev_ocf_get_bdevs', aliases=['get_ocf_bdevs'],
                              help='Get list of OCF devices including unregistered ones')
    p.add_argument('name', nargs='?', default=None, help='name of OCF vbdev or name of cache device or name of core device (optional)')
    p.set_defaults(func=bdev_ocf_get_bdevs)

    def bdev_malloc_create(args):
        num_blocks = (args.total_size * 1024 * 1024) // args.block_size
        print_json(rpc.bdev.bdev_malloc_create(args.client,
                                               num_blocks=int(num_blocks),
                                               block_size=args.block_size,
                                               name=args.name,
                                               uuid=args.uuid))
    p = subparsers.add_parser('bdev_malloc_create', aliases=['construct_malloc_bdev'],
                              help='Create a bdev with malloc backend')
    p.add_argument('-b', '--name', help="Name of the bdev")
    p.add_argument('-u', '--uuid', help="UUID of the bdev")
    p.add_argument(
        'total_size', help='Size of malloc bdev in MB (float > 0)', type=float)
    p.add_argument('block_size', help='Block size for this bdev', type=int)
    p.set_defaults(func=bdev_malloc_create)

    def bdev_malloc_delete(args):
        rpc.bdev.bdev_malloc_delete(args.client,
                                    name=args.name)

    p = subparsers.add_parser('bdev_malloc_delete', aliases=['delete_malloc_bdev'],
                              help='Delete a malloc disk')
    p.add_argument('name', help='malloc bdev name')
    p.set_defaults(func=bdev_malloc_delete)

    def bdev_null_create(args):
        num_blocks = (args.total_size * 1024 * 1024) // args.block_size
        print_json(rpc.bdev.bdev_null_create(args.client,
                                             num_blocks=num_blocks,
                                             block_size=args.block_size,
                                             name=args.name,
                                             uuid=args.uuid,
                                             md_size=args.md_size,
                                             dif_type=args.dif_type,
                                             dif_is_head_of_md=args.dif_is_head_of_md))

    p = subparsers.add_parser('bdev_null_create', aliases=['construct_null_bdev'],
                              help='Add a bdev with null backend')
    p.add_argument('name', help='Block device name')
    p.add_argument('-u', '--uuid', help='UUID of the bdev')
    p.add_argument(
        'total_size', help='Size of null bdev in MB (int > 0)', type=int)
    p.add_argument('block_size', help='Block size for this bdev', type=int)
    p.add_argument('-m', '--md-size', type=int,
                   help='Metadata size for this bdev. Default 0')
    p.add_argument('-t', '--dif-type', type=int, choices=[0, 1, 2, 3],
                   help='Protection information type. Default: 0 - no protection')
    p.add_argument('-d', '--dif-is-head-of-md', action='store_true',
                   help='Protection information is in the first 8 bytes of metadata. Default: in the last 8 bytes')
    p.set_defaults(func=bdev_null_create)

    def bdev_null_delete(args):
        rpc.bdev.bdev_null_delete(args.client,
                                  name=args.name)

    p = subparsers.add_parser('bdev_null_delete', aliases=['delete_null_bdev'],
                              help='Delete a null bdev')
    p.add_argument('name', help='null bdev name')
    p.set_defaults(func=bdev_null_delete)

    def bdev_aio_create(args):
        print_json(rpc.bdev.bdev_aio_create(args.client,
                                            filename=args.filename,
                                            name=args.name,
                                            block_size=args.block_size))

    p = subparsers.add_parser('bdev_aio_create', aliases=['construct_aio_bdev'],
                              help='Add a bdev with aio backend')
    p.add_argument('filename', help='Path to device or file (ex: /dev/sda)')
    p.add_argument('name', help='Block device name')
    p.add_argument('block_size', help='Block size for this bdev', type=int, nargs='?', default=0)
    p.set_defaults(func=bdev_aio_create)

    def bdev_aio_delete(args):
        rpc.bdev.bdev_aio_delete(args.client,
                                 name=args.name)

    p = subparsers.add_parser('bdev_aio_delete', aliases=['delete_aio_bdev'],
                              help='Delete an aio disk')
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
    p.add_argument('block_size', help='Block size for this bdev', type=int, nargs='?', default=0)
    p.set_defaults(func=bdev_uring_create)

    def bdev_uring_delete(args):
        rpc.bdev.bdev_uring_delete(args.client,
                                   name=args.name)

    p = subparsers.add_parser('bdev_uring_delete', help='Delete a uring bdev')
    p.add_argument('name', help='uring bdev name')
    p.set_defaults(func=bdev_uring_delete)

    def bdev_nvme_set_options(args):
        rpc.bdev.bdev_nvme_set_options(args.client,
                                       action_on_timeout=args.action_on_timeout,
                                       timeout_us=args.timeout_us,
                                       retry_count=args.retry_count,
                                       arbitration_burst=args.arbitration_burst,
                                       low_priority_weight=args.low_priority_weight,
                                       medium_priority_weight=args.medium_priority_weight,
                                       high_priority_weight=args.high_priority_weight,
                                       nvme_adminq_poll_period_us=args.nvme_adminq_poll_period_us,
                                       nvme_ioq_poll_period_us=args.nvme_ioq_poll_period_us,
                                       io_queue_requests=args.io_queue_requests,
                                       delay_cmd_submit=args.delay_cmd_submit)

    p = subparsers.add_parser('bdev_nvme_set_options', aliases=['set_bdev_nvme_options'],
                              help='Set options for the bdev nvme type. This is startup command.')
    p.add_argument('-a', '--action-on-timeout',
                   help="Action to take on command time out. Valid valies are: none, reset, abort")
    p.add_argument('-t', '--timeout-us',
                   help="Timeout for each command, in microseconds. If 0, don't track timeouts.", type=int)
    p.add_argument('-n', '--retry-count',
                   help='the number of attempts per I/O when an I/O fails', type=int)
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
                   action='store_false', dest='delay_cmd_submit', default=True)
    p.set_defaults(func=bdev_nvme_set_options)

    def bdev_nvme_set_hotplug(args):
        rpc.bdev.bdev_nvme_set_hotplug(args.client, enable=args.enable, period_us=args.period_us)

    p = subparsers.add_parser('bdev_nvme_set_hotplug', aliases=['set_bdev_nvme_hotplug'],
                              help='Set hotplug options for bdev nvme type.')
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
                                                         subnqn=args.subnqn,
                                                         hostnqn=args.hostnqn,
                                                         hostaddr=args.hostaddr,
                                                         hostsvcid=args.hostsvcid,
                                                         prchk_reftag=args.prchk_reftag,
                                                         prchk_guard=args.prchk_guard))

    p = subparsers.add_parser('bdev_nvme_attach_controller', aliases=['construct_nvme_bdev'],
                              help='Add bdevs with nvme backend')
    p.add_argument('-b', '--name', help="Name of the NVMe controller, prefix for each bdev name", required=True)
    p.add_argument('-t', '--trtype',
                   help='NVMe-oF target trtype: e.g., rdma, pcie', required=True)
    p.add_argument('-a', '--traddr',
                   help='NVMe-oF target address: e.g., an ip address or BDF', required=True)
    p.add_argument('-f', '--adrfam',
                   help='NVMe-oF target adrfam: e.g., ipv4, ipv6, ib, fc, intra_host')
    p.add_argument('-s', '--trsvcid',
                   help='NVMe-oF target trsvcid: e.g., a port number')
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
    p.set_defaults(func=bdev_nvme_attach_controller)

    def bdev_nvme_get_controllers(args):
        print_dict(rpc.nvme.bdev_nvme_get_controllers(args.client,
                                                      name=args.name))

    p = subparsers.add_parser(
        'bdev_nvme_get_controllers', aliases=['get_nvme_controllers'],
        help='Display current NVMe controllers list or required NVMe controller')
    p.add_argument('-n', '--name', help="Name of the NVMe controller. Example: Nvme0", required=False)
    p.set_defaults(func=bdev_nvme_get_controllers)

    def bdev_nvme_detach_controller(args):
        rpc.bdev.bdev_nvme_detach_controller(args.client,
                                             name=args.name)

    p = subparsers.add_parser('bdev_nvme_detach_controller', aliases=['delete_nvme_controller'],
                              help='Detach an NVMe controller and delete any associated bdevs')
    p.add_argument('name', help="Name of the controller")
    p.set_defaults(func=bdev_nvme_detach_controller)

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
                                            block_size=args.block_size))

    p = subparsers.add_parser('bdev_rbd_create', aliases=['construct_rbd_bdev'],
                              help='Add a bdev with ceph rbd backend')
    p.add_argument('-b', '--name', help="Name of the bdev", required=False)
    p.add_argument('--user', help="Ceph user name (i.e. admin, not client.admin)", required=False)
    p.add_argument('--config', action='append', metavar='key=value',
                   help="adds a key=value configuration option for rados_conf_set (default: rely on config file)")
    p.add_argument('pool_name', help='rbd pool name')
    p.add_argument('rbd_name', help='rbd image name')
    p.add_argument('block_size', help='rbd block size', type=int)
    p.set_defaults(func=bdev_rbd_create)

    def bdev_rbd_delete(args):
        rpc.bdev.bdev_rbd_delete(args.client,
                                 name=args.name)

    p = subparsers.add_parser('bdev_rbd_delete', aliases=['delete_rbd_bdev'],
                              help='Delete a rbd bdev')
    p.add_argument('name', help='rbd bdev name')
    p.set_defaults(func=bdev_rbd_delete)

    def bdev_delay_create(args):
        print_json(rpc.bdev.bdev_delay_create(args.client,
                                              base_bdev_name=args.base_bdev_name,
                                              name=args.name,
                                              avg_read_latency=args.avg_read_latency,
                                              p99_read_latency=args.nine_nine_read_latency,
                                              avg_write_latency=args.avg_write_latency,
                                              p99_write_latency=args.nine_nine_write_latency))

    p = subparsers.add_parser('bdev_delay_create',
                              help='Add a delay bdev on existing bdev')
    p.add_argument('-b', '--base-bdev-name', help="Name of the existing bdev", required=True)
    p.add_argument('-d', '--name', help="Name of the delay bdev", required=True)
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
                                              base_name=args.base_name))

    p = subparsers.add_parser('bdev_error_create', aliases=['construct_error_bdev'],
                              help='Add bdev with error injection backend')
    p.add_argument('base_name', help='base bdev name')
    p.set_defaults(func=bdev_error_create)

    def bdev_error_delete(args):
        rpc.bdev.bdev_error_delete(args.client,
                                   name=args.name)

    p = subparsers.add_parser('bdev_error_delete', aliases=['delete_error_bdev'],
                              help='Delete an error bdev')
    p.add_argument('name', help='error bdev name')
    p.set_defaults(func=bdev_error_delete)

    def bdev_iscsi_create(args):
        print_json(rpc.bdev.bdev_iscsi_create(args.client,
                                              name=args.name,
                                              url=args.url,
                                              initiator_iqn=args.initiator_iqn))

    p = subparsers.add_parser('bdev_iscsi_create', aliases=['construct_iscsi_bdev'],
                              help='Add bdev with iSCSI initiator backend')
    p.add_argument('-b', '--name', help="Name of the bdev", required=True)
    p.add_argument('-i', '--initiator-iqn', help="Initiator IQN", required=True)
    p.add_argument('--url', help="iSCSI Lun URL", required=True)
    p.set_defaults(func=bdev_iscsi_create)

    def bdev_iscsi_delete(args):
        rpc.bdev.bdev_iscsi_delete(args.client,
                                   name=args.name)

    p = subparsers.add_parser('bdev_iscsi_delete', aliases=['delete_iscsi_bdev'],
                              help='Delete an iSCSI bdev')
    p.add_argument('name', help='iSCSI bdev name')
    p.set_defaults(func=bdev_iscsi_delete)

    def bdev_pmem_create(args):
        print_json(rpc.bdev.bdev_pmem_create(args.client,
                                             pmem_file=args.pmem_file,
                                             name=args.name))

    p = subparsers.add_parser('bdev_pmem_create', aliases=['construct_pmem_bdev'],
                              help='Add a bdev with pmem backend')
    p.add_argument('pmem_file', help='Path to pmemblk pool file')
    p.add_argument('-n', '--name', help='Block device name', required=True)
    p.set_defaults(func=bdev_pmem_create)

    def bdev_pmem_delete(args):
        rpc.bdev.bdev_pmem_delete(args.client,
                                  name=args.name)

    p = subparsers.add_parser('bdev_pmem_delete', aliases=['delete_pmem_bdev'],
                              help='Delete a pmem bdev')
    p.add_argument('name', help='pmem bdev name')
    p.set_defaults(func=bdev_pmem_delete)

    def bdev_passthru_create(args):
        print_json(rpc.bdev.bdev_passthru_create(args.client,
                                                 base_bdev_name=args.base_bdev_name,
                                                 name=args.name))

    p = subparsers.add_parser('bdev_passthru_create', aliases=['construct_passthru_bdev'],
                              help='Add a pass through bdev on existing bdev')
    p.add_argument('-b', '--base-bdev-name', help="Name of the existing bdev", required=True)
    p.add_argument('-p', '--name', help="Name of the pass through bdev", required=True)
    p.set_defaults(func=bdev_passthru_create)

    def bdev_passthru_delete(args):
        rpc.bdev.bdev_passthru_delete(args.client,
                                      name=args.name)

    p = subparsers.add_parser('bdev_passthru_delete', aliases=['delete_passthru_bdev'],
                              help='Delete a pass through bdev')
    p.add_argument('name', help='pass through bdev name')
    p.set_defaults(func=bdev_passthru_delete)

    def bdev_get_bdevs(args):
        print_dict(rpc.bdev.bdev_get_bdevs(args.client,
                                           name=args.name))

    p = subparsers.add_parser('bdev_get_bdevs', aliases=['get_bdevs'],
                              help='Display current blockdev list or required blockdev')
    p.add_argument('-b', '--name', help="Name of the Blockdev. Example: Nvme0n1", required=False)
    p.set_defaults(func=bdev_get_bdevs)

    def bdev_get_iostat(args):
        print_dict(rpc.bdev.bdev_get_iostat(args.client,
                                            name=args.name))

    p = subparsers.add_parser('bdev_get_iostat', aliases=['get_bdevs_iostat'],
                              help='Display current I/O statistics of all the blockdevs or required blockdev.')
    p.add_argument('-b', '--name', help="Name of the Blockdev. Example: Nvme0n1", required=False)
    p.set_defaults(func=bdev_get_iostat)

    def bdev_enable_histogram(args):
        rpc.bdev.bdev_enable_histogram(args.client, name=args.name, enable=args.enable)

    p = subparsers.add_parser('bdev_enable_histogram', aliases=['enable_bdev_histogram'],
                              help='Enable or disable histogram for specified bdev')
    p.add_argument('-e', '--enable', default=True, dest='enable', action='store_true', help='Enable histograms on specified device')
    p.add_argument('-d', '--disable', dest='enable', action='store_false', help='Disable histograms on specified device')
    p.add_argument('name', help='bdev name')
    p.set_defaults(func=bdev_enable_histogram)

    def bdev_get_histogram(args):
        print_dict(rpc.bdev.bdev_get_histogram(args.client, name=args.name))

    p = subparsers.add_parser('bdev_get_histogram', aliases=['get_bdev_histogram'],
                              help='Get histogram for specified bdev')
    p.add_argument('name', help='bdev name')
    p.set_defaults(func=bdev_get_histogram)

    def bdev_set_qd_sampling_period(args):
        rpc.bdev.bdev_set_qd_sampling_period(args.client,
                                             name=args.name,
                                             period=args.period)

    p = subparsers.add_parser('bdev_set_qd_sampling_period', aliases=['set_bdev_qd_sampling_period'],
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

    p = subparsers.add_parser('bdev_set_qos_limit', aliases=['set_bdev_qos_limit'],
                              help='Set QoS rate limit on a blockdev')
    p.add_argument('name', help='Blockdev name to set QoS. Example: Malloc0')
    p.add_argument('--rw_ios_per_sec',
                   help='R/W IOs per second limit (>=10000, example: 20000). 0 means unlimited.',
                   type=int, required=False)
    p.add_argument('--rw_mbytes_per_sec',
                   help="R/W megabytes per second limit (>=10, example: 100). 0 means unlimited.",
                   type=int, required=False)
    p.add_argument('--r_mbytes_per_sec',
                   help="Read megabytes per second limit (>=10, example: 100). 0 means unlimited.",
                   type=int, required=False)
    p.add_argument('--w_mbytes_per_sec',
                   help="Write megabytes per second limit (>=10, example: 100). 0 means unlimited.",
                   type=int, required=False)
    p.set_defaults(func=bdev_set_qos_limit)

    def bdev_error_inject_error(args):
        rpc.bdev.bdev_error_inject_error(args.client,
                                         name=args.name,
                                         io_type=args.io_type,
                                         error_type=args.error_type,
                                         num=args.num)

    p = subparsers.add_parser('bdev_error_inject_error', aliases=['bdev_inject_error'],
                              help='bdev inject error')
    p.add_argument('name', help="""the name of the error injection bdev""")
    p.add_argument('io_type', help="""io_type: 'clear' 'read' 'write' 'unmap' 'flush' 'all'""")
    p.add_argument('error_type', help="""error_type: 'failure' 'pending'""")
    p.add_argument(
        '-n', '--num', help='the number of commands you want to fail', type=int, default=1)
    p.set_defaults(func=bdev_error_inject_error)

    def bdev_nvme_apply_firmware(args):
        print_dict(rpc.bdev.bdev_nvme_apply_firmware(args.client,
                                                     bdev_name=args.bdev_name,
                                                     filename=args.filename))

    p = subparsers.add_parser('apply_firmware', aliases=['apply_firmware'],
                              help='Download and commit firmware to NVMe device')
    p.add_argument('filename', help='filename of the firmware to download')
    p.add_argument('bdev_name', help='name of the NVMe device')
    p.set_defaults(func=bdev_nvme_apply_firmware)

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
            allow_duplicated_isid=args.allow_duplicated_isid)

    p = subparsers.add_parser('iscsi_set_options', aliases=['set_iscsi_options'],
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
    p.set_defaults(func=iscsi_set_options)

    def iscsi_set_discovery_auth(args):
        rpc.iscsi.iscsi_set_discovery_auth(
            args.client,
            disable_chap=args.disable_chap,
            require_chap=args.require_chap,
            mutual_chap=args.mutual_chap,
            chap_group=args.chap_group)

    p = subparsers.add_parser('iscsi_set_discovery_auth', aliases=['set_iscsi_discovery_auth'],
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

    p = subparsers.add_parser('iscsi_create_auth_group', aliases=['add_iscsi_auth_group'],
                              help='Create authentication group for CHAP authentication.')
    p.add_argument('tag', help='Authentication group tag (unique, integer > 0).', type=int)
    p.add_argument('-c', '--secrets', help="""Comma-separated list of CHAP secrets
<user:user_name secret:chap_secret muser:mutual_user_name msecret:mutual_chap_secret> enclosed in quotes.
Format: 'user:u1 secret:s1 muser:mu1 msecret:ms1,user:u2 secret:s2 muser:mu2 msecret:ms2'""", required=False)
    p.set_defaults(func=iscsi_create_auth_group)

    def iscsi_delete_auth_group(args):
        rpc.iscsi.iscsi_delete_auth_group(args.client, tag=args.tag)

    p = subparsers.add_parser('iscsi_delete_auth_group', aliases=['delete_iscsi_auth_group'],
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

    p = subparsers.add_parser('iscsi_auth_group_add_secret', aliases=['add_secret_to_iscsi_auth_group'],
                              help='Add a secret to an authentication group.')
    p.add_argument('tag', help='Authentication group tag', type=int)
    p.add_argument('-u', '--user', help='User name for one-way CHAP authentication', required=True)
    p.add_argument('-s', '--secret', help='Secret for one-way CHAP authentication', required=True)
    p.add_argument('-m', '--muser', help='User name for mutual CHAP authentication')
    p.add_argument('-r', '--msecret', help='Secret for mutual CHAP authentication')
    p.set_defaults(func=iscsi_auth_group_add_secret)

    def iscsi_auth_group_remove_secret(args):
        rpc.iscsi.iscsi_auth_group_remove_secret(args.client, tag=args.tag, user=args.user)

    p = subparsers.add_parser('iscsi_auth_group_remove_secret', aliases=['delete_secret_from_iscsi_auth_group'],
                              help='Remove a secret from an authentication group.')
    p.add_argument('tag', help='Authentication group tag', type=int)
    p.add_argument('-u', '--user', help='User name for one-way CHAP authentication', required=True)
    p.set_defaults(func=iscsi_auth_group_remove_secret)

    def iscsi_get_auth_groups(args):
        print_dict(rpc.iscsi.iscsi_get_auth_groups(args.client))

    p = subparsers.add_parser('iscsi_get_auth_groups', aliases=['get_iscsi_auth_groups'],
                              help='Display current authentication group configuration')
    p.set_defaults(func=iscsi_get_auth_groups)

    def iscsi_get_portal_groups(args):
        print_dict(rpc.iscsi.iscsi_get_portal_groups(args.client))

    p = subparsers.add_parser(
        'iscsi_get_portal_groups', aliases=['get_portal_groups'],
        help='Display current portal group configuration')
    p.set_defaults(func=iscsi_get_portal_groups)

    def iscsi_get_initiator_groups(args):
        print_dict(rpc.iscsi.iscsi_get_initiator_groups(args.client))

    p = subparsers.add_parser('iscsi_get_initiator_groups',
                              aliases=['get_initiator_groups'],
                              help='Display current initiator group configuration')
    p.set_defaults(func=iscsi_get_initiator_groups)

    def iscsi_get_target_nodes(args):
        print_dict(rpc.iscsi.iscsi_get_target_nodes(args.client))

    p = subparsers.add_parser('iscsi_get_target_nodes', aliases=['get_target_nodes'],
                              help='Display target nodes')
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

    p = subparsers.add_parser('iscsi_create_target_node', aliases=['construct_target_node'],
                              help='Add a target node')
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
    *** Authentication group must be precreated ***""", type=int, default=0)
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

    p = subparsers.add_parser('iscsi_target_node_add_lun', aliases=['target_node_add_lun'],
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

    p = subparsers.add_parser('iscsi_target_node_set_auth', aliases=['set_iscsi_target_node_auth'],
                              help='Set CHAP authentication for the target node')
    p.add_argument('name', help='Target node name (ASCII)')
    p.add_argument('-g', '--chap-group', help="""Authentication group ID for this target node.
    *** Authentication group must be precreated ***""", type=int, default=0)
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
                              aliases=['add_pg_ig_maps'],
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
                              aliases=['delete_pg_ig_maps'],
                              help='Delete PG-IG maps from the target node')
    p.add_argument('name', help='Target node name (ASCII)')
    p.add_argument('pg_ig_mappings', help="""List of (Portal_Group_Tag:Initiator_Group_Tag) mappings
    Whitespace separated, quoted, mapping defined with colon
    separated list of "tags" (int > 0)
    Example: '1:1 2:2 2:1'
    *** The Portal/Initiator Groups must be precreated ***""")
    p.set_defaults(func=iscsi_target_node_remove_pg_ig_maps)

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
            tag=args.tag)

    p = subparsers.add_parser('iscsi_create_portal_group', aliases=['add_portal_group'],
                              help='Add a portal group')
    p.add_argument(
        'tag', help='Portal group tag (unique, integer > 0)', type=int)
    p.add_argument('portal_list', help="""List of portals in host:port format, separated by whitespace
    Example: '192.168.100.100:3260 192.168.100.100:3261 192.168.100.100:3262""")
    p.set_defaults(func=iscsi_create_portal_group)

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

    p = subparsers.add_parser('iscsi_create_initiator_group', aliases=['add_initiator_group'],
                              help='Add an initiator group')
    p.add_argument(
        'tag', help='Initiator group tag (unique, integer > 0)', type=int)
    p.add_argument('initiator_list', help="""Whitespace-separated list of initiator hostnames or IP addresses,
    enclosed in quotes.  Example: 'ANY' or '127.0.0.1 192.168.200.100'""")
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
                              aliases=['add_initiators_to_initiator_group'],
                              help='Add initiators to an existing initiator group')
    p.add_argument(
        'tag', help='Initiator group tag (unique, integer > 0)', type=int)
    p.add_argument('-n', dest='initiator_list', help="""Whitespace-separated list of initiator hostnames or IP addresses,
    enclosed in quotes.  This parameter can be omitted.  Example: 'ANY' or '127.0.0.1 192.168.200.100'""", required=False)
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
                              aliases=['delete_initiators_from_initiator_group'],
                              help='Delete initiators from an existing initiator group')
    p.add_argument(
        'tag', help='Initiator group tag (unique, integer > 0)', type=int)
    p.add_argument('-n', dest='initiator_list', help="""Whitespace-separated list of initiator hostnames or IP addresses,
    enclosed in quotes.  This parameter can be omitted.  Example: 'ANY' or '127.0.0.1 192.168.200.100'""", required=False)
    p.add_argument('-m', dest='netmask_list', help="""Whitespace-separated list of initiator netmasks enclosed in quotes.
    This parameter can be omitted.  Example: '255.255.0.0 255.248.0.0' etc""", required=False)
    p.set_defaults(func=iscsi_initiator_group_remove_initiators)

    def iscsi_delete_target_node(args):
        rpc.iscsi.iscsi_delete_target_node(
            args.client, target_node_name=args.target_node_name)

    p = subparsers.add_parser('iscsi_delete_target_node', aliases=['delete_target_node'],
                              help='Delete a target node')
    p.add_argument('target_node_name',
                   help='Target node name to be deleted. Example: iqn.2016-06.io.spdk:disk1.')
    p.set_defaults(func=iscsi_delete_target_node)

    def iscsi_delete_portal_group(args):
        rpc.iscsi.iscsi_delete_portal_group(args.client, tag=args.tag)

    p = subparsers.add_parser('iscsi_delete_portal_group',
                              aliases=['delete_portal_group'],
                              help='Delete a portal group')
    p.add_argument(
        'tag', help='Portal group tag (unique, integer > 0)', type=int)
    p.set_defaults(func=iscsi_delete_portal_group)

    def iscsi_delete_initiator_group(args):
        rpc.iscsi.iscsi_delete_initiator_group(args.client, tag=args.tag)

    p = subparsers.add_parser('iscsi_delete_initiator_group',
                              aliases=['delete_initiator_group'],
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
    *** Authentication group must be precreated ***""", type=int, default=0)
    p.add_argument('-d', '--disable-chap', help="""CHAP authentication should be disabled for this portal group.
    *** Mutually exclusive with --require-chap ***""", action='store_true')
    p.add_argument('-r', '--require-chap', help="""CHAP authentication should be required for this portal group.
    *** Mutually exclusive with --disable-chap ***""", action='store_true')
    p.add_argument('-m', '--mutual-chap', help='CHAP authentication should be mutual/bidirectional.',
                   action='store_true')
    p.set_defaults(func=iscsi_portal_group_set_auth)

    def iscsi_get_connections(args):
        print_dict(rpc.iscsi.iscsi_get_connections(args.client))

    p = subparsers.add_parser('iscsi_get_connections', aliases=['get_iscsi_connections'],
                              help='Display iSCSI connections')
    p.set_defaults(func=iscsi_get_connections)

    def iscsi_get_options(args):
        print_dict(rpc.iscsi.iscsi_get_options(args.client))

    p = subparsers.add_parser('iscsi_get_options', aliases=['get_iscsi_global_params'],
                              help='Display iSCSI global parameters')
    p.set_defaults(func=iscsi_get_options)

    def scsi_get_devices(args):
        print_dict(rpc.iscsi.scsi_get_devices(args.client))

    p = subparsers.add_parser('scsi_get_devices', aliases=['get_scsi_devices'],
                              help='Display SCSI devices')
    p.set_defaults(func=scsi_get_devices)

    # trace
    def trace_enable_tpoint_group(args):
        rpc.trace.trace_enable_tpoint_group(args.client, name=args.name)

    p = subparsers.add_parser('trace_enable_tpoint_group', aliases=['enable_tpoint_group'],
                              help='enable trace on a specific tpoint group')
    p.add_argument(
        'name', help="""trace group name we want to enable in tpoint_group_mask.
        (for example "bdev" for bdev trace group, "all" for all trace groups).""")
    p.set_defaults(func=trace_enable_tpoint_group)

    def trace_disable_tpoint_group(args):
        rpc.trace.trace_disable_tpoint_group(args.client, name=args.name)

    p = subparsers.add_parser('trace_disable_tpoint_group', aliases=['disable_tpoint_group'],
                              help='disable trace on a specific tpoint group')
    p.add_argument(
        'name', help="""trace group name we want to disable in tpoint_group_mask.
        (for example "bdev" for bdev trace group, "all" for all trace groups).""")
    p.set_defaults(func=trace_disable_tpoint_group)

    def trace_get_tpoint_group_mask(args):
        print_dict(rpc.trace.trace_get_tpoint_group_mask(args.client))

    p = subparsers.add_parser('trace_get_tpoint_group_mask', aliases=['get_tpoint_group_mask'],
                              help='get trace point group mask')
    p.set_defaults(func=trace_get_tpoint_group_mask)

    # log
    def log_set_flag(args):
        rpc.log.log_set_flag(args.client, flag=args.flag)

    p = subparsers.add_parser('log_set_flag', help='set log flag', aliases=['set_log_flag'])
    p.add_argument(
        'flag', help='log flag we want to set. (for example "nvme").')
    p.set_defaults(func=log_set_flag)

    def log_clear_flag(args):
        rpc.log.log_clear_flag(args.client, flag=args.flag)

    p = subparsers.add_parser('log_clear_flag', help='clear log flag', aliases=['clear_log_flag'])
    p.add_argument(
        'flag', help='log flag we want to clear. (for example "nvme").')
    p.set_defaults(func=log_clear_flag)

    def log_get_flags(args):
        print_dict(rpc.log.log_get_flags(args.client))

    p = subparsers.add_parser('log_get_flags', help='get log flags', aliases=['get_log_flags'])
    p.set_defaults(func=log_get_flags)

    def log_set_level(args):
        rpc.log.log_set_level(args.client, level=args.level)

    p = subparsers.add_parser('log_set_level', aliases=['set_log_level'],
                              help='set log level')
    p.add_argument('level', help='log level we want to set. (for example "DEBUG").')
    p.set_defaults(func=log_set_level)

    def log_get_level(args):
        print_dict(rpc.log.log_get_level(args.client))

    p = subparsers.add_parser('log_get_level', aliases=['get_log_level'],
                              help='get log level')
    p.set_defaults(func=log_get_level)

    def log_set_print_level(args):
        rpc.log.log_set_print_level(args.client, level=args.level)

    p = subparsers.add_parser('log_set_print_level', aliases=['set_log_print_level'],
                              help='set log print level')
    p.add_argument('level', help='log print level we want to set. (for example "DEBUG").')
    p.set_defaults(func=log_set_print_level)

    def log_get_print_level(args):
        print_dict(rpc.log.log_get_print_level(args.client))

    p = subparsers.add_parser('log_get_print_level', aliases=['get_log_print_level'],
                              help='get log print level')
    p.set_defaults(func=log_get_print_level)

    # lvol
    def bdev_lvol_create_lvstore(args):
        print_json(rpc.lvol.bdev_lvol_create_lvstore(args.client,
                                                     bdev_name=args.bdev_name,
                                                     lvs_name=args.lvs_name,
                                                     cluster_sz=args.cluster_sz,
                                                     clear_method=args.clear_method))

    p = subparsers.add_parser('bdev_lvol_create_lvstore', aliases=['construct_lvol_store'],
                              help='Add logical volume store on base bdev')
    p.add_argument('bdev_name', help='base bdev name')
    p.add_argument('lvs_name', help='name for lvol store')
    p.add_argument('-c', '--cluster-sz', help='size of cluster (in bytes)', type=int, required=False)
    p.add_argument('--clear-method', help="""Change clear method for data region.
        Available: none, unmap, write_zeroes""", required=False)
    p.set_defaults(func=bdev_lvol_create_lvstore)

    def bdev_lvol_rename_lvstore(args):
        rpc.lvol.bdev_lvol_rename_lvstore(args.client,
                                          old_name=args.old_name,
                                          new_name=args.new_name)

    p = subparsers.add_parser('bdev_lvol_rename_lvstore', aliases=['rename_lvol_store'],
                              help='Change logical volume store name')
    p.add_argument('old_name', help='old name')
    p.add_argument('new_name', help='new name')
    p.set_defaults(func=bdev_lvol_rename_lvstore)

    def bdev_lvol_create(args):
        print_json(rpc.lvol.bdev_lvol_create(args.client,
                                             lvol_name=args.lvol_name,
                                             size=args.size * 1024 * 1024,
                                             thin_provision=args.thin_provision,
                                             clear_method=args.clear_method,
                                             uuid=args.uuid,
                                             lvs_name=args.lvs_name))

    p = subparsers.add_parser('bdev_lvol_create', aliases=['construct_lvol_bdev'],
                              help='Add a bdev with an logical volume backend')
    p.add_argument('-u', '--uuid', help='lvol store UUID', required=False)
    p.add_argument('-l', '--lvs-name', help='lvol store name', required=False)
    p.add_argument('-t', '--thin-provision', action='store_true', help='create lvol bdev as thin provisioned')
    p.add_argument('-c', '--clear-method', help="""Change default data clusters clear method.
        Available: none, unmap, write_zeroes""", required=False)
    p.add_argument('lvol_name', help='name for this lvol')
    p.add_argument('size', help='size in MiB for this bdev', type=int)
    p.set_defaults(func=bdev_lvol_create)

    def bdev_lvol_snapshot(args):
        print_json(rpc.lvol.bdev_lvol_snapshot(args.client,
                                               lvol_name=args.lvol_name,
                                               snapshot_name=args.snapshot_name))

    p = subparsers.add_parser('bdev_lvol_snapshot', aliases=['snapshot_lvol_bdev'],
                              help='Create a snapshot of an lvol bdev')
    p.add_argument('lvol_name', help='lvol bdev name')
    p.add_argument('snapshot_name', help='lvol snapshot name')
    p.set_defaults(func=bdev_lvol_snapshot)

    def bdev_lvol_clone(args):
        print_json(rpc.lvol.bdev_lvol_clone(args.client,
                                            snapshot_name=args.snapshot_name,
                                            clone_name=args.clone_name))

    p = subparsers.add_parser('bdev_lvol_clone', aliases=['clone_lvol_bdev'],
                              help='Create a clone of an lvol snapshot')
    p.add_argument('snapshot_name', help='lvol snapshot name')
    p.add_argument('clone_name', help='lvol clone name')
    p.set_defaults(func=bdev_lvol_clone)

    def bdev_lvol_rename(args):
        rpc.lvol.bdev_lvol_rename(args.client,
                                  old_name=args.old_name,
                                  new_name=args.new_name)

    p = subparsers.add_parser('bdev_lvol_rename', aliases=['rename_lvol_bdev'],
                              help='Change lvol bdev name')
    p.add_argument('old_name', help='lvol bdev name')
    p.add_argument('new_name', help='new lvol name')
    p.set_defaults(func=bdev_lvol_rename)

    def bdev_lvol_inflate(args):
        rpc.lvol.bdev_lvol_inflate(args.client,
                                   name=args.name)

    p = subparsers.add_parser('bdev_lvol_inflate', aliases=['inflate_lvol_bdev'],
                              help='Make thin provisioned lvol a thick provisioned lvol')
    p.add_argument('name', help='lvol bdev name')
    p.set_defaults(func=bdev_lvol_inflate)

    def bdev_lvol_decouple_parent(args):
        rpc.lvol.bdev_lvol_decouple_parent(args.client,
                                           name=args.name)

    p = subparsers.add_parser('bdev_lvol_decouple_parent', aliases=['decouple_parent_lvol_bdev'],
                              help='Decouple parent of lvol')
    p.add_argument('name', help='lvol bdev name')
    p.set_defaults(func=bdev_lvol_decouple_parent)

    def bdev_lvol_resize(args):
        rpc.lvol.bdev_lvol_resize(args.client,
                                  name=args.name,
                                  size=args.size * 1024 * 1024)

    p = subparsers.add_parser('bdev_lvol_resize', aliases=['resize_lvol_bdev'],
                              help='Resize existing lvol bdev')
    p.add_argument('name', help='lvol bdev name')
    p.add_argument('size', help='new size in MiB for this bdev', type=int)
    p.set_defaults(func=bdev_lvol_resize)

    def bdev_lvol_set_read_only(args):
        rpc.lvol.bdev_lvol_set_read_only(args.client,
                                         name=args.name)

    p = subparsers.add_parser('bdev_lvol_set_read_only', aliases=['set_read_only_lvol_bdev'],
                              help='Mark lvol bdev as read only')
    p.add_argument('name', help='lvol bdev name')
    p.set_defaults(func=bdev_lvol_set_read_only)

    def bdev_lvol_delete(args):
        rpc.lvol.bdev_lvol_delete(args.client,
                                  name=args.name)

    p = subparsers.add_parser('bdev_lvol_delete', aliases=['destroy_lvol_bdev'],
                              help='Destroy a logical volume')
    p.add_argument('name', help='lvol bdev name')
    p.set_defaults(func=bdev_lvol_delete)

    def bdev_lvol_delete_lvstore(args):
        rpc.lvol.bdev_lvol_delete_lvstore(args.client,
                                          uuid=args.uuid,
                                          lvs_name=args.lvs_name)

    p = subparsers.add_parser('bdev_lvol_delete_lvstore', aliases=['destroy_lvol_store'],
                              help='Destroy an logical volume store')
    p.add_argument('-u', '--uuid', help='lvol store UUID', required=False)
    p.add_argument('-l', '--lvs-name', help='lvol store name', required=False)
    p.set_defaults(func=bdev_lvol_delete_lvstore)

    def bdev_lvol_get_lvstores(args):
        print_dict(rpc.lvol.bdev_lvol_get_lvstores(args.client,
                                                   uuid=args.uuid,
                                                   lvs_name=args.lvs_name))

    p = subparsers.add_parser('bdev_lvol_get_lvstores', aliases=['get_lvol_stores'],
                              help='Display current logical volume store list')
    p.add_argument('-u', '--uuid', help='lvol store UUID', required=False)
    p.add_argument('-l', '--lvs-name', help='lvol store name', required=False)
    p.set_defaults(func=bdev_lvol_get_lvstores)

    def bdev_raid_get_bdevs(args):
        print_array(rpc.bdev.bdev_raid_get_bdevs(args.client,
                                                 category=args.category))

    p = subparsers.add_parser('bdev_raid_get_bdevs', aliases=['get_raid_bdevs'],
                              help="""This is used to list all the raid bdev names based on the input category
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
                                  strip_size=args.strip_size,
                                  strip_size_kb=args.strip_size_kb,
                                  raid_level=args.raid_level,
                                  base_bdevs=base_bdevs)
    p = subparsers.add_parser('bdev_raid_create', aliases=['construct_raid_bdev'],
                              help='Create new raid bdev')
    p.add_argument('-n', '--name', help='raid bdev name', required=True)
    p.add_argument('-s', '--strip-size', help='strip size in KB (deprecated)', type=int)
    p.add_argument('-z', '--strip-size_kb', help='strip size in KB', type=int)
    p.add_argument('-r', '--raid-level', help='raid level, only raid level 0 is supported', required=True)
    p.add_argument('-b', '--base-bdevs', help='base bdevs name, whitespace separated list in quotes', required=True)
    p.set_defaults(func=bdev_raid_create)

    def bdev_raid_delete(args):
        rpc.bdev.bdev_raid_delete(args.client,
                                  name=args.name)
    p = subparsers.add_parser('bdev_raid_delete', aliases=['destroy_raid_bdev'],
                              help='Delete existing raid bdev')
    p.add_argument('name', help='raid bdev name')
    p.set_defaults(func=bdev_raid_delete)

    # split
    def bdev_split_create(args):
        print_array(rpc.bdev.bdev_split_create(args.client,
                                               base_bdev=args.base_bdev,
                                               split_count=args.split_count,
                                               split_size_mb=args.split_size_mb))

    p = subparsers.add_parser('bdev_split_create', aliases=['construct_split_vbdev'],
                              help="""Add given disk name to split config. If bdev with base_name
    name exist the split bdevs will be created right away, if not split bdevs will be created when base bdev became
    available (during examination process).""")
    p.add_argument('base_bdev', help='base bdev name')
    p.add_argument('-s', '--split-size-mb', help='size in MiB for each bdev', type=int, default=0)
    p.add_argument('split_count', help="""Optional - number of split bdevs to create. Total size * split_count must not
    exceed the base bdev size.""", type=int)
    p.set_defaults(func=bdev_split_create)

    def bdev_split_delete(args):
        rpc.bdev.bdev_split_delete(args.client,
                                   base_bdev=args.base_bdev)

    p = subparsers.add_parser('bdev_split_delete', aliases=['destruct_split_vbdev'],
                              help="""Delete split config with all created splits.""")
    p.add_argument('base_bdev', help='base bdev name')
    p.set_defaults(func=bdev_split_delete)

    # ftl
    ftl_valid_limits = ('crit', 'high', 'low', 'start')

    def bdev_ftl_create(args):
        def parse_limits(limits, arg_dict, key_suffix=''):
            for limit in limits.split(','):
                key, value = limit.split(':', 1)
                if key in ftl_valid_limits:
                    arg_dict['limit_' + key + key_suffix] = int(value)
                else:
                    raise ValueError('Limit {} is not supported'.format(key))

        arg_limits = {}
        if args.limit_threshold:
            parse_limits(args.limit_threshold, arg_limits, '_threshold')

        if args.limit:
            parse_limits(args.limit, arg_limits)

        print_dict(rpc.bdev.bdev_ftl_create(args.client,
                                            name=args.name,
                                            base_bdev=args.base_bdev,
                                            uuid=args.uuid,
                                            cache=args.cache,
                                            allow_open_bands=args.allow_open_bands,
                                            overprovisioning=args.overprovisioning,
                                            **arg_limits))

    p = subparsers.add_parser('bdev_ftl_create', aliases=['construct_ftl_bdev'], help='Add FTL bdev')
    p.add_argument('-b', '--name', help="Name of the bdev", required=True)
    p.add_argument('-d', '--base_bdev', help='Name of zoned bdev used as underlying device',
                   required=True)
    p.add_argument('-u', '--uuid', help='UUID of restored bdev (not applicable when creating new '
                   'instance): e.g. b286d19a-0059-4709-abcd-9f7732b1567d (optional)')
    p.add_argument('-c', '--cache', help='Name of the bdev to be used as a write buffer cache (optional)')
    p.add_argument('-o', '--allow_open_bands', help='Restoring after dirty shutdown without cache will'
                   ' result in partial data recovery, instead of error', action='store_true')
    p.add_argument('--overprovisioning', help='Percentage of device used for relocation, not exposed'
                   ' to user (optional)', type=int)

    limits = p.add_argument_group('Defrag limits', 'Configures defrag limits and thresholds for'
                                  ' levels ' + str(ftl_valid_limits)[1:-1])
    limits.add_argument('--limit', help='Percentage of allowed user versus internal writes at given'
                        ' levels, e.g. crit:0,high:20,low:80')
    limits.add_argument('--limit-threshold', help='Number of free bands triggering a given level of'
                        ' write limiting e.g. crit:1,high:2,low:3,start:4')
    p.set_defaults(func=bdev_ftl_create)

    def bdev_ftl_delete(args):
        print_dict(rpc.bdev.bdev_ftl_delete(args.client, name=args.name))

    p = subparsers.add_parser('bdev_ftl_delete', aliases=['delete_ftl_bdev'],
                              help='Delete FTL bdev')
    p.add_argument('-b', '--name', help="Name of the bdev", required=True)
    p.set_defaults(func=bdev_ftl_delete)

    # vmd
    def enable_vmd(args):
        print_dict(rpc.vmd.enable_vmd(args.client))

    p = subparsers.add_parser('enable_vmd', help='Enable VMD enumeration')
    p.set_defaults(func=enable_vmd)

    # nbd
    def nbd_start_disk(args):
        print(rpc.nbd.nbd_start_disk(args.client,
                                     bdev_name=args.bdev_name,
                                     nbd_device=args.nbd_device))

    p = subparsers.add_parser('nbd_start_disk', aliases=['start_nbd_disk'],
                              help='Export a bdev as an nbd disk')
    p.add_argument('bdev_name', help='Blockdev name to be exported. Example: Malloc0.')
    p.add_argument('nbd_device', help='Nbd device name to be assigned. Example: /dev/nbd0.', nargs='?')
    p.set_defaults(func=nbd_start_disk)

    def nbd_stop_disk(args):
        rpc.nbd.nbd_stop_disk(args.client,
                              nbd_device=args.nbd_device)

    p = subparsers.add_parser('nbd_stop_disk', aliases=['stop_nbd_disk'],
                              help='Stop an nbd disk')
    p.add_argument('nbd_device', help='Nbd device name to be stopped. Example: /dev/nbd0.')
    p.set_defaults(func=nbd_stop_disk)

    def nbd_get_disks(args):
        print_dict(rpc.nbd.nbd_get_disks(args.client,
                                         nbd_device=args.nbd_device))

    p = subparsers.add_parser('nbd_get_disks', aliases=['get_nbd_disks'],
                              help='Display full or specified nbd device list')
    p.add_argument('-n', '--nbd-device', help="Path of the nbd device. Example: /dev/nbd0", required=False)
    p.set_defaults(func=nbd_get_disks)

    # net
    def net_interface_add_ip_address(args):
        rpc.net.net_interface_add_ip_address(args.client, ifc_index=args.ifc_index, ip_addr=args.ip_addr)

    p = subparsers.add_parser('net_interface_add_ip_address', aliases=['add_ip_address'],
                              help='Add IP address')
    p.add_argument('ifc_index', help='ifc index of the nic device.', type=int)
    p.add_argument('ip_addr', help='ip address will be added.')
    p.set_defaults(func=net_interface_add_ip_address)

    def net_interface_delete_ip_address(args):
        rpc.net.net_interface_delete_ip_address(args.client, ifc_index=args.ifc_index, ip_addr=args.ip_addr)

    p = subparsers.add_parser('net_interface_delete_ip_address', aliases=['delete_ip_address'],
                              help='Delete IP address')
    p.add_argument('ifc_index', help='ifc index of the nic device.', type=int)
    p.add_argument('ip_addr', help='ip address will be deleted.')
    p.set_defaults(func=net_interface_delete_ip_address)

    def net_get_interfaces(args):
        print_dict(rpc.net.net_get_interfaces(args.client))

    p = subparsers.add_parser(
        'net_get_interfaces', aliases=['get_interfaces'], help='Display current interface list')
    p.set_defaults(func=net_get_interfaces)

    # NVMe-oF
    def nvmf_set_max_subsystems(args):
        rpc.nvmf.nvmf_set_max_subsystems(args.client,
                                         max_subsystems=args.max_subsystems)

    p = subparsers.add_parser('nvmf_set_max_subsystems', aliases=['set_nvmf_target_max_subsystems'],
                              help='Set the maximum number of NVMf target subsystems')
    p.add_argument('-x', '--max-subsystems', help='Max number of NVMf subsystems', type=int, required=True)
    p.set_defaults(func=nvmf_set_max_subsystems)

    def nvmf_set_config(args):
        rpc.nvmf.nvmf_set_config(args.client,
                                 acceptor_poll_rate=args.acceptor_poll_rate,
                                 conn_sched=args.conn_sched)

    p = subparsers.add_parser('nvmf_set_config', aliases=['set_nvmf_target_config'],
                              help='Set NVMf target config')
    p.add_argument('-r', '--acceptor-poll-rate', help='Polling interval of the acceptor for incoming connections (usec)', type=int)
    p.add_argument('-s', '--conn-sched', help="""'roundrobin' - Schedule the incoming connections from any host
    on the cores in a round robin manner (Default). 'hostip' - Schedule all the incoming connections from a
    specific host IP on to the same core. Connections from different IP will be assigned to cores in a round
    robin manner. 'transport' - Schedule the connection according to the transport characteristics.""")
    p.set_defaults(func=nvmf_set_config)

    def nvmf_create_transport(args):
        rpc.nvmf.nvmf_create_transport(args.client,
                                       trtype=args.trtype,
                                       tgt_name=args.tgt_name,
                                       max_queue_depth=args.max_queue_depth,
                                       max_qpairs_per_ctrlr=args.max_qpairs_per_ctrlr,
                                       in_capsule_data_size=args.in_capsule_data_size,
                                       max_io_size=args.max_io_size,
                                       io_unit_size=args.io_unit_size,
                                       max_aq_depth=args.max_aq_depth,
                                       num_shared_buffers=args.num_shared_buffers,
                                       buf_cache_size=args.buf_cache_size,
                                       max_srq_depth=args.max_srq_depth,
                                       no_srq=args.no_srq,
                                       c2h_success=args.c2h_success,
                                       dif_insert_or_strip=args.dif_insert_or_strip,
                                       sock_priority=args.sock_priority)

    p = subparsers.add_parser('nvmf_create_transport', help='Create NVMf transport')
    p.add_argument('-t', '--trtype', help='Transport type (ex. RDMA)', type=str, required=True)
    p.add_argument('-g', '--tgt_name', help='The name of the parent NVMe-oF target (optional)', type=str)
    p.add_argument('-q', '--max-queue-depth', help='Max number of outstanding I/O per queue', type=int)
    p.add_argument('-p', '--max-qpairs-per-ctrlr', help='Max number of SQ and CQ per controller', type=int)
    p.add_argument('-c', '--in-capsule-data-size', help='Max number of in-capsule data size', type=int)
    p.add_argument('-i', '--max-io-size', help='Max I/O size (bytes)', type=int)
    p.add_argument('-u', '--io-unit-size', help='I/O unit size (bytes)', type=int)
    p.add_argument('-a', '--max-aq-depth', help='Max number of admin cmds per AQ', type=int)
    p.add_argument('-n', '--num-shared-buffers', help='The number of pooled data buffers available to the transport', type=int)
    p.add_argument('-b', '--buf-cache-size', help='The number of shared buffers to reserve for each poll group', type=int)
    p.add_argument('-s', '--max-srq-depth', help='Max number of outstanding I/O per SRQ. Relevant only for RDMA transport', type=int)
    p.add_argument('-r', '--no-srq', action='store_true', help='Disable per-thread shared receive queue. Relevant only for RDMA transport')
    p.add_argument('-o', '--c2h-success', action='store_false', help='Disable C2H success optimization. Relevant only for TCP transport')
    p.add_argument('-f', '--dif-insert-or-strip', action='store_true', help='Enable DIF insert/strip. Relevant only for TCP transport')
    p.add_argument('-y', '--sock-priority', help='The sock priority of the tcp connection. Relevant only for TCP transport', type=int)
    p.set_defaults(func=nvmf_create_transport)

    def nvmf_get_transports(args):
        print_dict(rpc.nvmf.nvmf_get_transports(args.client, tgt_name=args.tgt_name))

    p = subparsers.add_parser('nvmf_get_transports', aliases=['get_nvmf_transports'],
                              help='Display nvmf transports')
    p.add_argument('-t', '--tgt_name', help='The name of the parent NVMe-oF target (optional)', type=str)
    p.set_defaults(func=nvmf_get_transports)

    def nvmf_get_subsystems(args):
        print_dict(rpc.nvmf.nvmf_get_subsystems(args.client, tgt_name=args.tgt_name))

    p = subparsers.add_parser('nvmf_get_subsystems', aliases=['get_nvmf_subsystems'],
                              help='Display nvmf subsystems')
    p.add_argument('-t', '--tgt_name', help='The name of the parent NVMe-oF target (optional)', type=str)
    p.set_defaults(func=nvmf_get_subsystems)

    def nvmf_create_subsystem(args):
        rpc.nvmf.nvmf_create_subsystem(args.client,
                                       nqn=args.nqn,
                                       tgt_name=args.tgt_name,
                                       serial_number=args.serial_number,
                                       model_number=args.model_number,
                                       allow_any_host=args.allow_any_host,
                                       max_namespaces=args.max_namespaces)

    p = subparsers.add_parser('nvmf_create_subsystem', aliases=['nvmf_subsystem_create'],
                              help='Create an NVMe-oF subsystem')
    p.add_argument('nqn', help='Subsystem NQN (ASCII)')
    p.add_argument('-t', '--tgt_name', help='The name of the parent NVMe-oF target (optional)', type=str)
    p.add_argument("-s", "--serial-number", help="""
    Format:  'sn' etc
    Example: 'SPDK00000000000001'""", default='00000000000000000000')
    p.add_argument("-d", "--model-number", help="""
    Format:  'mn' etc
    Example: 'SPDK Controller'""", default='SPDK bdev Controller')
    p.add_argument("-a", "--allow-any-host", action='store_true', help="Allow any host to connect (don't enforce host NQN whitelist)")
    p.add_argument("-m", "--max-namespaces", help="Maximum number of namespaces allowed",
                   type=int, default=0)
    p.set_defaults(func=nvmf_create_subsystem)

    def nvmf_delete_subsystem(args):
        rpc.nvmf.nvmf_delete_subsystem(args.client,
                                       nqn=args.subsystem_nqn,
                                       tgt_name=args.tgt_name)

    p = subparsers.add_parser('nvmf_delete_subsystem', aliases=['delete_nvmf_subsystem'],
                              help='Delete a nvmf subsystem')
    p.add_argument('subsystem_nqn',
                   help='subsystem nqn to be deleted. Example: nqn.2016-06.io.spdk:cnode1.')
    p.add_argument('-t', '--tgt_name', help='The name of the parent NVMe-oF target (optional)', type=str)
    p.set_defaults(func=nvmf_delete_subsystem)

    def nvmf_subsystem_add_listener(args):
        rpc.nvmf.nvmf_subsystem_add_listener(args.client,
                                             nqn=args.nqn,
                                             trtype=args.trtype,
                                             traddr=args.traddr,
                                             tgt_name=args.tgt_name,
                                             adrfam=args.adrfam,
                                             trsvcid=args.trsvcid)

    p = subparsers.add_parser('nvmf_subsystem_add_listener', help='Add a listener to an NVMe-oF subsystem')
    p.add_argument('nqn', help='NVMe-oF subsystem NQN')
    p.add_argument('-t', '--trtype', help='NVMe-oF transport type: e.g., rdma', required=True)
    p.add_argument('-a', '--traddr', help='NVMe-oF transport address: e.g., an ip address', required=True)
    p.add_argument('-p', '--tgt_name', help='The name of the parent NVMe-oF target (optional)', type=str)
    p.add_argument('-f', '--adrfam', help='NVMe-oF transport adrfam: e.g., ipv4, ipv6, ib, fc, intra_host')
    p.add_argument('-s', '--trsvcid', help='NVMe-oF transport service id: e.g., a port number')
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
    p.add_argument('nqn', help='NVMe-oF subsystem NQN')
    p.add_argument('-t', '--trtype', help='NVMe-oF transport type: e.g., rdma', required=True)
    p.add_argument('-a', '--traddr', help='NVMe-oF transport address: e.g., an ip address', required=True)
    p.add_argument('-p', '--tgt_name', help='The name of the parent NVMe-oF target (optional)', type=str)
    p.add_argument('-f', '--adrfam', help='NVMe-oF transport adrfam: e.g., ipv4, ipv6, ib, fc, intra_host')
    p.add_argument('-s', '--trsvcid', help='NVMe-oF transport service id: e.g., a port number')
    p.set_defaults(func=nvmf_subsystem_remove_listener)

    def nvmf_subsystem_add_ns(args):
        rpc.nvmf.nvmf_subsystem_add_ns(args.client,
                                       nqn=args.nqn,
                                       bdev_name=args.bdev_name,
                                       tgt_name=args.tgt_name,
                                       ptpl_file=args.ptpl_file,
                                       nsid=args.nsid,
                                       nguid=args.nguid,
                                       eui64=args.eui64,
                                       uuid=args.uuid)

    p = subparsers.add_parser('nvmf_subsystem_add_ns', help='Add a namespace to an NVMe-oF subsystem')
    p.add_argument('nqn', help='NVMe-oF subsystem NQN')
    p.add_argument('bdev_name', help='The name of the bdev that will back this namespace')
    p.add_argument('-t', '--tgt_name', help='The name of the parent NVMe-oF target (optional)', type=str)
    p.add_argument('-p', '--ptpl-file', help='The persistent reservation storage location (optional)', type=str)
    p.add_argument('-n', '--nsid', help='The requested NSID (optional)', type=int)
    p.add_argument('-g', '--nguid', help='Namespace globally unique identifier (optional)')
    p.add_argument('-e', '--eui64', help='Namespace EUI-64 identifier (optional)')
    p.add_argument('-u', '--uuid', help='Namespace UUID (optional)')
    p.set_defaults(func=nvmf_subsystem_add_ns)

    def nvmf_subsystem_remove_ns(args):
        rpc.nvmf.nvmf_subsystem_remove_ns(args.client,
                                          nqn=args.nqn,
                                          nsid=args.nsid,
                                          tgt_name=args.tgt_name)

    p = subparsers.add_parser('nvmf_subsystem_remove_ns', help='Remove a namespace to an NVMe-oF subsystem')
    p.add_argument('nqn', help='NVMe-oF subsystem NQN')
    p.add_argument('nsid', help='The requested NSID', type=int)
    p.add_argument('-t', '--tgt_name', help='The name of the parent NVMe-oF target (optional)', type=str)
    p.set_defaults(func=nvmf_subsystem_remove_ns)

    def nvmf_subsystem_add_host(args):
        rpc.nvmf.nvmf_subsystem_add_host(args.client,
                                         nqn=args.nqn,
                                         host=args.host,
                                         tgt_name=args.tgt_name)

    p = subparsers.add_parser('nvmf_subsystem_add_host', help='Add a host to an NVMe-oF subsystem')
    p.add_argument('nqn', help='NVMe-oF subsystem NQN')
    p.add_argument('host', help='Host NQN to allow')
    p.add_argument('-t', '--tgt_name', help='The name of the parent NVMe-oF target (optional)', type=str)
    p.set_defaults(func=nvmf_subsystem_add_host)

    def nvmf_subsystem_remove_host(args):
        rpc.nvmf.nvmf_subsystem_remove_host(args.client,
                                            nqn=args.nqn,
                                            host=args.host,
                                            tgt_name=args.tgt_name)

    p = subparsers.add_parser('nvmf_subsystem_remove_host', help='Remove a host from an NVMe-oF subsystem')
    p.add_argument('nqn', help='NVMe-oF subsystem NQN')
    p.add_argument('host', help='Host NQN to remove')
    p.add_argument('-t', '--tgt_name', help='The name of the parent NVMe-oF target (optional)', type=str)
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
    p.add_argument('-t', '--tgt_name', help='The name of the parent NVMe-oF target (optional)', type=str)
    p.set_defaults(func=nvmf_subsystem_allow_any_host)

    def nvmf_get_stats(args):
        print_dict(rpc.nvmf.nvmf_get_stats(args.client, tgt_name=args.tgt_name))

    p = subparsers.add_parser(
        'nvmf_get_stats', help='Display current statistics for NVMf subsystem')
    p.add_argument('-t', '--tgt_name', help='The name of the parent NVMe-oF target (optional)', type=str)
    p.set_defaults(func=nvmf_get_stats)

    # pmem
    def bdev_pmem_create_pool(args):
        num_blocks = int((args.total_size * 1024 * 1024) / args.block_size)
        rpc.pmem.bdev_pmem_create_pool(args.client,
                                       pmem_file=args.pmem_file,
                                       num_blocks=num_blocks,
                                       block_size=args.block_size)

    p = subparsers.add_parser('bdev_pmem_create_pool', aliases=['create_pmem_pool'],
                              help='Create pmem pool')
    p.add_argument('pmem_file', help='Path to pmemblk pool file')
    p.add_argument('total_size', help='Size of malloc bdev in MB (int > 0)', type=int)
    p.add_argument('block_size', help='Block size for this pmem pool', type=int)
    p.set_defaults(func=bdev_pmem_create_pool)

    def bdev_pmem_get_pool_info(args):
        print_dict(rpc.pmem.bdev_pmem_get_pool_info(args.client,
                                                    pmem_file=args.pmem_file))

    p = subparsers.add_parser('bdev_pmem_get_pool_info', aliases=['pmem_pool_info'],
                              help='Display pmem pool info and check consistency')
    p.add_argument('pmem_file', help='Path to pmemblk pool file')
    p.set_defaults(func=bdev_pmem_get_pool_info)

    def bdev_pmem_delete_pool(args):
        rpc.pmem.bdev_pmem_delete_pool(args.client,
                                       pmem_file=args.pmem_file)

    p = subparsers.add_parser('bdev_pmem_delete_pool', aliases=['delete_pmem_pool'],
                              help='Delete pmem pool')
    p.add_argument('pmem_file', help='Path to pmemblk pool file')
    p.set_defaults(func=bdev_pmem_delete_pool)

    # subsystem
    def framework_get_subsystems(args):
        print_dict(rpc.subsystem.framework_get_subsystems(args.client))

    p = subparsers.add_parser('framework_get_subsystems', aliases=['get_subsystems'],
                              help="""Print subsystems array in initialization order. Each subsystem
    entry contain (unsorted) array of subsystems it depends on.""")
    p.set_defaults(func=framework_get_subsystems)

    def framework_get_config(args):
        print_dict(rpc.subsystem.framework_get_config(args.client, args.name))

    p = subparsers.add_parser('framework_get_config', aliases=['get_subsystem_config'],
                              help="""Print subsystem configuration""")
    p.add_argument('name', help='Name of subsystem to query')
    p.set_defaults(func=framework_get_config)

    # vhost
    def vhost_controller_set_coalescing(args):
        rpc.vhost.vhost_controller_set_coalescing(args.client,
                                                  ctrlr=args.ctrlr,
                                                  delay_base_us=args.delay_base_us,
                                                  iops_threshold=args.iops_threshold)

    p = subparsers.add_parser('vhost_controller_set_coalescing', aliases=['set_vhost_controller_coalescing'],
                              help='Set vhost controller coalescing')
    p.add_argument('ctrlr', help='controller name')
    p.add_argument('delay_base_us', help='Base delay time', type=int)
    p.add_argument('iops_threshold', help='IOPS threshold when coalescing is enabled', type=int)
    p.set_defaults(func=vhost_controller_set_coalescing)

    def vhost_create_scsi_controller(args):
        rpc.vhost.vhost_create_scsi_controller(args.client,
                                               ctrlr=args.ctrlr,
                                               cpumask=args.cpumask)

    p = subparsers.add_parser(
        'vhost_create_scsi_controller', aliases=['construct_vhost_scsi_controller'],
        help='Add new vhost controller')
    p.add_argument('ctrlr', help='controller name')
    p.add_argument('--cpumask', help='cpu mask for this controller')
    p.set_defaults(func=vhost_create_scsi_controller)

    def vhost_scsi_controller_add_target(args):
        print_json(rpc.vhost.vhost_scsi_controller_add_target(args.client,
                                                              ctrlr=args.ctrlr,
                                                              scsi_target_num=args.scsi_target_num,
                                                              bdev_name=args.bdev_name))

    p = subparsers.add_parser('vhost_scsi_controller_add_target',
                              aliases=['add_vhost_scsi_lun'],
                              help='Add lun to vhost controller')
    p.add_argument('ctrlr', help='conntroller name where add lun')
    p.add_argument('scsi_target_num', help='scsi_target_num', type=int)
    p.add_argument('bdev_name', help='bdev name')
    p.set_defaults(func=vhost_scsi_controller_add_target)

    def vhost_scsi_controller_remove_target(args):
        rpc.vhost.vhost_scsi_controller_remove_target(args.client,
                                                      ctrlr=args.ctrlr,
                                                      scsi_target_num=args.scsi_target_num)

    p = subparsers.add_parser('vhost_scsi_controller_remove_target',
                              aliases=['remove_vhost_scsi_target'],
                              help='Remove target from vhost controller')
    p.add_argument('ctrlr', help='controller name to remove target from')
    p.add_argument('scsi_target_num', help='scsi_target_num', type=int)
    p.set_defaults(func=vhost_scsi_controller_remove_target)

    def vhost_create_blk_controller(args):
        rpc.vhost.vhost_create_blk_controller(args.client,
                                              ctrlr=args.ctrlr,
                                              dev_name=args.dev_name,
                                              cpumask=args.cpumask,
                                              readonly=args.readonly)

    p = subparsers.add_parser('vhost_create_blk_controller',
                              aliases=['construct_vhost_blk_controller'],
                              help='Add a new vhost block controller')
    p.add_argument('ctrlr', help='controller name')
    p.add_argument('dev_name', help='device name')
    p.add_argument('--cpumask', help='cpu mask for this controller')
    p.add_argument("-r", "--readonly", action='store_true', help='Set controller as read-only')
    p.set_defaults(func=vhost_create_blk_controller)

    def vhost_create_nvme_controller(args):
        rpc.vhost.vhost_create_nvme_controller(args.client,
                                               ctrlr=args.ctrlr,
                                               io_queues=args.io_queues,
                                               cpumask=args.cpumask)

    p = subparsers.add_parser('vhost_create_nvme_controller', aliases=['vhost_create_nvme_controller'],
                              help='Add new vhost controller')
    p.add_argument('ctrlr', help='controller name')
    p.add_argument('io_queues', help='number of IO queues for the controller', type=int)
    p.add_argument('--cpumask', help='cpu mask for this controller')
    p.set_defaults(func=vhost_create_nvme_controller)

    def vhost_nvme_controller_add_ns(args):
        rpc.vhost.vhost_nvme_controller_add_ns(args.client,
                                               ctrlr=args.ctrlr,
                                               bdev_name=args.bdev_name)

    p = subparsers.add_parser('vhost_nvme_controller_add_ns', aliases=['add_vhost_nvme_ns'],
                              help='Add a Namespace to vhost controller')
    p.add_argument('ctrlr', help='conntroller name where add a Namespace')
    p.add_argument('bdev_name', help='block device name for a new Namespace')
    p.set_defaults(func=vhost_nvme_controller_add_ns)

    def vhost_get_controllers(args):
        print_dict(rpc.vhost.vhost_get_controllers(args.client, args.name))

    p = subparsers.add_parser('vhost_get_controllers', aliases=['get_vhost_controllers'],
                              help='List all or specific vhost controller(s)')
    p.add_argument('-n', '--name', help="Name of vhost controller", required=False)
    p.set_defaults(func=vhost_get_controllers)

    def vhost_delete_controller(args):
        rpc.vhost.vhost_delete_controller(args.client,
                                          ctrlr=args.ctrlr)

    p = subparsers.add_parser('vhost_delete_controller', aliases=['remove_vhost_controller'],
                              help='Delete a vhost controller')
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

    p = subparsers.add_parser('bdev_virtio_attach_controller', aliases=['construct_virtio_dev'],
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

    p = subparsers.add_parser('bdev_virtio_scsi_get_devices', aliases=['get_virtio_scsi_devs'],
                              help='List all Virtio-SCSI devices.')
    p.set_defaults(func=bdev_virtio_scsi_get_devices)

    def bdev_virtio_detach_controller(args):
        rpc.vhost.bdev_virtio_detach_controller(args.client,
                                                name=args.name)

    p = subparsers.add_parser('bdev_virtio_detach_controller', aliases=['remove_virtio_bdev'],
                              help="""Remove a Virtio device
    This will delete all bdevs exposed by this device""")
    p.add_argument('name', help='Virtio device name. E.g. VirtioUser0')
    p.set_defaults(func=bdev_virtio_detach_controller)

    # OCSSD
    def bdev_ocssd_create(args):
        nsid = int(args.nsid) if args.nsid is not None else None
        print_json(rpc.bdev.bdev_ocssd_create(args.client,
                                              ctrlr_name=args.ctrlr_name,
                                              bdev_name=args.name,
                                              nsid=nsid,
                                              range=args.range))

    p = subparsers.add_parser('bdev_ocssd_create',
                              help='Creates zoned bdev on specified Open Channel controller')
    p.add_argument('-c', '--ctrlr_name', help='Name of the OC NVMe controller', required=True)
    p.add_argument('-b', '--name', help='Name of the bdev to create', required=True)
    p.add_argument('-n', '--nsid', help='Namespace ID', required=False)
    p.add_argument('-r', '--range', help='Parallel unit range (in the form of BEGIN-END (inclusive))',
                   required=False)
    p.set_defaults(func=bdev_ocssd_create)

    def bdev_ocssd_delete(args):
        print_json(rpc.bdev.bdev_ocssd_delete(args.client,
                                              name=args.name))

    p = subparsers.add_parser('bdev_ocssd_delete',
                              help='Deletes Open Channel bdev')
    p.add_argument('name', help='Name of the Open Channel bdev')
    p.set_defaults(func=bdev_ocssd_delete)

    # ioat
    def ioat_scan_copy_engine(args):
        pci_whitelist = []
        if args.pci_whitelist:
            for w in args.pci_whitelist.strip().split(" "):
                pci_whitelist.append(w)
        rpc.ioat.ioat_scan_copy_engine(args.client, pci_whitelist)

    p = subparsers.add_parser('ioat_scan_copy_engine', aliases=['scan_ioat_copy_engine'],
                              help='Set scan and enable IOAT copy engine offload.')
    p.add_argument('-w', '--pci-whitelist', help="""Whitespace-separated list of PCI addresses in
    domain:bus:device.function format or domain.bus.device.function format""")
    p.set_defaults(func=ioat_scan_copy_engine)

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

    p = subparsers.add_parser('bdev_nvme_send_cmd', aliases=['send_nvme_cmd'],
                              help='NVMe passthrough cmd.')
    p.add_argument('-n', '--nvme-name', help="""Name of the operating NVMe controller""")
    p.add_argument('-t', '--cmd-type', help="""Type of nvme cmd. Valid values are: admin, io""")
    p.add_argument('-r', '--data-direction', help="""Direction of data transfer. Valid values are: c2h, h2c""")
    p.add_argument('-c', '--cmdbuf', help="""NVMe command encoded by base64 urlsafe""")
    p.add_argument('-d', '--data', help="""Data transferring to controller from host, encoded by base64 urlsafe""")
    p.add_argument('-m', '--metadata', help="""Metadata transferring to controller from host, encoded by base64 urlsafe""")
    p.add_argument('-D', '--data-length', help="""Data length required to transfer from controller to host""", type=int)
    p.add_argument('-M', '--metadata-length', help="""Metadata length required to transfer from controller to host""", type=int)
    p.add_argument('-T', '--timeout-ms',
                   help="""Command execution timeout value, in milliseconds,  if 0, don't track timeout""", type=int, default=0)
    p.set_defaults(func=bdev_nvme_send_cmd)

    # Notifications
    def notify_get_types(args):
        print_dict(rpc.notify.notify_get_types(args.client))

    p = subparsers.add_parser('notify_get_types', aliases=['get_notification_types'],
                              help='List available notifications that user can subscribe to.')
    p.set_defaults(func=notify_get_types)

    def notify_get_notifications(args):
        ret = rpc.notify.notify_get_notifications(args.client,
                                                  id=args.id,
                                                  max=args.max)
        print_dict(ret)

    p = subparsers.add_parser('notify_get_notifications', aliases=['get_notifications'],
                              help='Get notifications')
    p.add_argument('-i', '--id', help="""First ID to start fetching from""", type=int)
    p.add_argument('-n', '--max', help="""Maximum number of notifications to return in response""", type=int)
    p.set_defaults(func=notify_get_notifications)

    def thread_get_stats(args):
        print_dict(rpc.app.thread_get_stats(args.client))

    p = subparsers.add_parser(
        'thread_get_stats', help='Display current statistics of all the threads')
    p.set_defaults(func=thread_get_stats)

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
    p.add_argument('-c', '--cluster_sz',
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
            args = parser.parse_args(shlex.split(rpc_call))
            args.client = client
            try:
                call_rpc_func(args)
            except JSONRPCException as ex:
                print("Exception:")
                print(executed_rpc.strip() + " <<<")
                print(ex.message)
                exit(1)

    args = parser.parse_args()
    if args.dry_run:
        args.client = dry_run_client()
        print_dict = null_print
        print_json = null_print
        print_array = null_print
    else:
        args.client = rpc.client.JSONRPCClient(args.server_addr, args.port, args.timeout, log_level=getattr(logging, args.verbose.upper()))
    if hasattr(args, 'func'):
        try:
            call_rpc_func(args)
        except JSONRPCException as ex:
            print(ex.message)
            exit(1)
    elif sys.stdin.isatty():
        # No arguments and no data piped through stdin
        parser.print_help()
        exit(1)
    else:
        execute_script(parser, args.client, sys.stdin)
