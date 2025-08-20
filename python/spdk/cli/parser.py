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

    def framework_start_init(args):
        args.client.framework_start_init()

    p = subparsers.add_parser('framework_start_init', help='Start initialization of subsystems')
    p.set_defaults(func=framework_start_init)

    def framework_wait_init(args):
        args.client.framework_wait_init()

    p = subparsers.add_parser('framework_wait_init', help='Block until subsystems have been initialized')
    p.set_defaults(func=framework_wait_init)

    def rpc_get_methods(args):
        print_dict(args.client.rpc_get_methods(
                                       current=args.current,
                                       include_aliases=args.include_aliases))

    p = subparsers.add_parser('rpc_get_methods', help='Get list of supported RPC methods')
    p.add_argument('-c', '--current', help='Get list of RPC methods only callable in the current state.', action='store_true')
    p.add_argument('-i', '--include-aliases', help='include RPC aliases', action='store_true')
    p.set_defaults(func=rpc_get_methods)

    def spdk_get_version(args):
        print_json(args.client.spdk_get_version())

    p = subparsers.add_parser('spdk_get_version', help='Get SPDK version')
    p.set_defaults(func=spdk_get_version)

    def save_config(args):
        rpc.save_config(args.client,
                        fd=sys.stdout,
                        indent=args.indent,
                        subsystems=args.subsystems)

    p = subparsers.add_parser('save_config', help="""Write current (live) configuration of SPDK subsystems and targets to stdout.
    """)
    p.add_argument('-i', '--indent', help="""Indent level. Value less than 0 mean compact mode. Default indent level is 2.
    """, type=int, default=2)
    p.add_argument('-s', '--subsystems', help="""Comma-separated list of subsystems (and their dependencies) to save""")
    p.set_defaults(func=save_config)

    def load_config(args):
        rpc.load_config(args.client,
                        fd=args.json_conf,
                        include_aliases=args.include_aliases)

    p = subparsers.add_parser('load_config', help="""Configure SPDK subsystems and targets using JSON RPC.""")
    p.add_argument('-i', '--include-aliases', help='include RPC aliases', action='store_true')
    p.add_argument('-j', '--json-conf', help='Valid JSON configuration', default=sys.stdin)
    p.set_defaults(func=load_config)

    def save_subsystem_config(args):
        rpc.save_subsystem_config(args.client,
                                  fd=sys.stdout,
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
                                  fd=args.json_conf)

    p = subparsers.add_parser('load_subsystem_config', help="""Configure SPDK subsystem using JSON RPC.""")
    p.add_argument('-j', '--json-conf', help='Valid JSON configuration', default=sys.stdin)
    p.set_defaults(func=load_subsystem_config)

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

    # subsystem
    def framework_get_subsystems(args):
        print_dict(args.client.framework_get_subsystems())

    p = subparsers.add_parser('framework_get_subsystems',
                              help="""Print subsystems array in initialization order. Each subsystem
    entry contain (unsorted) array of subsystems it depends on.""")
    p.set_defaults(func=framework_get_subsystems)

    def framework_get_config(args):
        print_dict(args.client.framework_get_config(name=args.name))

    p = subparsers.add_parser('framework_get_config', help="""Print subsystem configuration""")
    p.add_argument('name', help='Name of subsystem to query')
    p.set_defaults(func=framework_get_config)

    # ioat
    def ioat_scan_accel_module(args):
        args.client.ioat_scan_accel_module()

    p = subparsers.add_parser('ioat_scan_accel_module', aliases=['ioat_scan_accel_engine'],
                              help='Enable IOAT accel module offload.')
    p.set_defaults(func=ioat_scan_accel_module)

    # dpdk compressdev
    def compressdev_scan_accel_module(args):
        args.client.compressdev_scan_accel_module(pmd=args.pmd)

    p = subparsers.add_parser('compressdev_scan_accel_module', help='Scan and enable compressdev module and set pmd option.')
    p.add_argument('-p', '--pmd', type=int, help='0 = auto-select, 1= QAT only, 2 = mlx5_pci only, 3 = uadk only')
    p.set_defaults(func=compressdev_scan_accel_module)

    # dsa
    def dsa_scan_accel_module(args):
        args.client.dsa_scan_accel_module(config_kernel_mode=args.config_kernel_mode)

    p = subparsers.add_parser('dsa_scan_accel_module', aliases=['dsa_scan_accel_engine'],
                              help='Set config and enable dsa accel module offload.')
    p.add_argument('-k', '--config-kernel-mode', help='Use Kernel mode dsa',
                   action='store_true', dest='config_kernel_mode')
    p.set_defaults(func=dsa_scan_accel_module, config_kernel_mode=None)

    # iaa
    def iaa_scan_accel_module(args):
        args.client.iaa_scan_accel_module()

    p = subparsers.add_parser('iaa_scan_accel_module', aliases=['iaa_scan_accel_engine'],
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
    p.add_argument('-d', '--driver-name', help='The driver, can be one of crypto_aesni_mb, crypto_qat or mlx5_pci', type=str)
    p.set_defaults(func=dpdk_cryptodev_set_driver)

    def dpdk_cryptodev_get_driver(args):
        print_dict(args.client.dpdk_cryptodev_get_driver())

    p = subparsers.add_parser('dpdk_cryptodev_get_driver', help='Get the DPDK cryptodev driver')
    p.set_defaults(func=dpdk_cryptodev_get_driver)

    # mlx5
    def mlx5_scan_accel_module(args):
        args.client.mlx5_scan_accel_module(
                                        qp_size=args.qp_size,
                                        num_requests=args.num_requests,
                                        allowed_devs=args.allowed_devs,
                                        crypto_split_blocks=args.crypto_split_blocks,
                                        enable_driver=args.enable_driver)

    p = subparsers.add_parser('mlx5_scan_accel_module', help='Enable mlx5 accel module.')
    p.add_argument('-q', '--qp-size', type=int, help='QP size')
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
    p.add_argument('-l', '--level', type=str, help='Verbose level, one of \"total\", \"channel\" or \"device\"')
    p.set_defaults(func=accel_mlx5_dump_stats)

    # accel_error
    def accel_error_inject_error(args):
        args.client.accel_error_inject_error(opcode=args.opcode,
                                             type=args.type, count=args.count,
                                             interval=args.interval, errcode=args.errcode)

    p = subparsers.add_parser('accel_error_inject_error',
                              help='Inject an error to processing accel operation')
    p.add_argument('-o', '--opcode', help='Opcode')
    p.add_argument('-t', '--type',
                   help='Error type ("corrupt": corrupt the data, "failure": fail the operation, "disable": disable error injection)')
    p.add_argument('-c', '--count', type=int,
                   help='Number of errors to inject on each IO channel (0 to disable error injection)')
    p.add_argument('-i', '--interval', type=int, help='Interval between injections')
    p.add_argument('--errcode', type=int, help='Error code to inject (only relevant for type=failure)')
    p.set_defaults(func=accel_error_inject_error)

    # opal
    def bdev_nvme_opal_init(args):
        args.client.bdev_nvme_opal_init(
                                     nvme_ctrlr_name=args.nvme_ctrlr_name,
                                     password=args.password)

    p = subparsers.add_parser('bdev_nvme_opal_init', help='take ownership and activate')
    p.add_argument('-b', '--nvme-ctrlr-name', help='nvme ctrlr name')
    p.add_argument('-p', '--password', help='password for admin')
    p.set_defaults(func=bdev_nvme_opal_init)

    def bdev_nvme_opal_revert(args):
        args.client.bdev_nvme_opal_revert(
                                       nvme_ctrlr_name=args.nvme_ctrlr_name,
                                       password=args.password)
    p = subparsers.add_parser('bdev_nvme_opal_revert', help='Revert to default factory settings')
    p.add_argument('-b', '--nvme-ctrlr-name', help='nvme ctrlr name')
    p.add_argument('-p', '--password', help='password')
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
    p.add_argument('-b', '--bdev-name', help='opal bdev')
    p.add_argument('-p', '--password', help='password')
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

    def thread_get_stats(args):
        print_dict(args.client.thread_get_stats())

    p = subparsers.add_parser(
        'thread_get_stats', help='Display current statistics of all the threads')
    p.set_defaults(func=thread_get_stats)

    def thread_set_cpumask(args):
        ret = args.client.thread_set_cpumask(
                                         id=args.id,
                                         cpumask=args.cpumask)
    p = subparsers.add_parser('thread_set_cpumask',
                              help="""set the cpumask of the thread whose ID matches to the
    specified value. The thread may be migrated to one of the specified CPUs.""")
    p.add_argument('-i', '--id', type=int, help='thread ID')
    p.add_argument('-m', '--cpumask', help='cpumask for this thread')
    p.set_defaults(func=thread_set_cpumask)

    def log_enable_timestamps(args):
        ret = args.client.log_enable_timestamps(enabled=args.enabled)
    p = subparsers.add_parser('log_enable_timestamps',
                              help='Enable or disable timestamps.')
    p.add_argument('-d', '--disable', dest='enabled', default=False, action='store_false', help="Disable timestamps")
    p.add_argument('-e', '--enable', dest='enabled', action='store_true', help="Enable timestamps")
    p.set_defaults(func=log_enable_timestamps)

    def thread_get_pollers(args):
        print_dict(args.client.thread_get_pollers())

    p = subparsers.add_parser(
        'thread_get_pollers', help='Display current pollers of all the threads')
    p.set_defaults(func=thread_get_pollers)

    def thread_get_io_channels(args):
        print_dict(args.client.thread_get_io_channels())

    p = subparsers.add_parser(
        'thread_get_io_channels', help='Display current IO channels of all the threads')
    p.set_defaults(func=thread_get_io_channels)

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

    # bdev_nvme_add_error_injection
    def bdev_nvme_add_error_injection(args):
        print_dict(args.client.bdev_nvme_add_error_injection(
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
        print_dict(args.client.bdev_nvme_remove_error_injection(
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
                                             new_size=int(args.new_size)))

    p = subparsers.add_parser('bdev_daos_resize',
                              help='Resize a DAOS bdev')
    p.add_argument('name', help='DAOS bdev name')
    p.add_argument('new_size', help='new bdev size for resize operation. The unit is MiB')
    p.set_defaults(func=bdev_daos_resize)
