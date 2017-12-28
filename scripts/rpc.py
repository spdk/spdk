#!/usr/bin/env python

import argparse
import rpc

if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description='SPDK RPC command line interface')
    parser.add_argument('-s', dest='server_addr',
                        help='RPC server address', default='/var/tmp/spdk.sock')
    parser.add_argument('-p', dest='port',
                        help='RPC port number (if server_addr is IP address)',
                        default=5260, type=int)
    parser.add_argument('-v', dest='verbose',
                        help='Verbose mode', action='store_true')
    subparsers = parser.add_subparsers(help='RPC methods')

    p = subparsers.add_parser('get_rpc_methods', help='Get list of supported RPC methods')
    p.set_defaults(func=rpc.get_rpc_methods)

    # app
    p = subparsers.add_parser('kill_instance', help='Send signal to instance')
    p.add_argument('sig_name', help='signal will be sent to server.')
    p.set_defaults(func=rpc.app.kill_instance)

    p = subparsers.add_parser('context_switch_monitor', help='Control whether the context switch monitor is enabled')
    p.add_argument('-e', '--enable', action='store_true', help='Enable context switch monitoring')
    p.add_argument('-d', '--disable', action='store_true', help='Disable context switch monitoring')
    p.set_defaults(func=rpc.app.context_switch_monitor)

    # bdev
    p = subparsers.add_parser('construct_malloc_bdev',
                              help='Add a bdev with malloc backend')
    p.add_argument('-b', '--name', help="Name of the bdev")
    p.add_argument(
        'total_size', help='Size of malloc bdev in MB (int > 0)', type=int)
    p.add_argument('block_size', help='Block size for this bdev', type=int)
    p.set_defaults(func=rpc.bdev.construct_malloc_bdev)

    p = subparsers.add_parser('construct_null_bdev',
                              help='Add a bdev with null backend')
    p.add_argument('name', help='Block device name')
    p.add_argument(
        'total_size', help='Size of null bdev in MB (int > 0)', type=int)
    p.add_argument('block_size', help='Block size for this bdev', type=int)
    p.set_defaults(func=rpc.bdev.construct_null_bdev)

    p = subparsers.add_parser('construct_aio_bdev',
                              help='Add a bdev with aio backend')
    p.add_argument('filename', help='Path to device or file (ex: /dev/sda)')
    p.add_argument('name', help='Block device name')
    p.add_argument('block_size', help='Block size for this bdev', type=int, default=argparse.SUPPRESS)
    p.set_defaults(func=rpc.bdev.construct_aio_bdev)

    p = subparsers.add_parser('construct_nvme_bdev',
                              help='Add bdev with nvme backend')
    p.add_argument('-b', '--name', help="Name of the bdev", required=True)
    p.add_argument('-t', '--trtype',
                   help='NVMe-oF target trtype: e.g., rdma, pcie', required=True)
    p.add_argument('-a', '--traddr',
                   help='NVMe-oF target address: e.g., an ip address or BDF', required=True)
    p.add_argument('-f', '--adrfam',
                   help='NVMe-oF target adrfam: e.g., ipv4, ipv6, ib, fc, intra_host')
    p.add_argument('-s', '--trsvcid',
                   help='NVMe-oF target trsvcid: e.g., a port number')
    p.add_argument('-n', '--subnqn', help='NVMe-oF target subnqn')
    p.set_defaults(func=rpc.bdev.construct_nvme_bdev)

    p = subparsers.add_parser('construct_rbd_bdev',
                              help='Add a bdev with ceph rbd backend')
    p.add_argument('pool_name', help='rbd pool name')
    p.add_argument('rbd_name', help='rbd image name')
    p.add_argument('block_size', help='rbd block size', type=int)
    p.set_defaults(func=rpc.bdev.construct_rbd_bdev)

    p = subparsers.add_parser('construct_error_bdev',
                              help='Add bdev with error injection backend')
    p.add_argument('base_name', help='base bdev name')
    p.set_defaults(func=rpc.bdev.construct_error_bdev)

    p = subparsers.add_parser('construct_pmem_bdev', help='Add a bdev with pmem backend')
    p.add_argument('pmem_file', help='Path to pmemblk pool file')
    p.add_argument('-n', '--name', help='Block device name', required=True)
    p.set_defaults(func=rpc.bdev.construct_pmem_bdev)

    p = subparsers.add_parser(
        'get_bdevs', help='Display current blockdev list or required blockdev')
    p.add_argument('-b', '--name', help="Name of the Blockdev. Example: Nvme0n1", required=False)
    p.set_defaults(func=rpc.bdev.get_bdevs)

    p = subparsers.add_parser(
        'get_bdevs_iostat', help='Display current I/O statistics of all the blockdevs or required blockdev.')
    p.add_argument('-b', '--name', help="Name of the Blockdev. Example: Nvme0n1", required=False)
    p.set_defaults(func=rpc.bdev.get_bdevs_iostat)

    p = subparsers.add_parser('delete_bdev', help='Delete a blockdev')
    p.add_argument(
        'bdev_name', help='Blockdev name to be deleted. Example: Malloc0.')
    p.set_defaults(func=rpc.bdev.delete_bdev)

    p = subparsers.add_parser('bdev_inject_error', help='bdev inject error')
    p.add_argument('name', help="""the name of the error injection bdev""")
    p.add_argument('io_type', help="""io_type: 'clear' 'read' 'write' 'unmap' 'flush' 'all'""")
    p.add_argument('error_type', help="""error_type: 'failure' 'pending'""")
    p.add_argument(
        '-n', '--num', help='the number of commands you want to fail', type=int, default=1)
    p.set_defaults(func=rpc.bdev.bdev_inject_error)

    p = subparsers.add_parser('apply_firmware', help='Download and commit firmware to NVMe device')
    p.add_argument('filename', help='filename of the firmware to download')
    p.add_argument('bdev_name', help='name of the NVMe device')
    p.set_defaults(func=rpc.bdev.apply_firmware)

    # iSCSI
    p = subparsers.add_parser(
        'get_portal_groups', help='Display current portal group configuration')
    p.set_defaults(func=rpc.iscsi.get_portal_groups)

    p = subparsers.add_parser('get_initiator_groups',
                              help='Display current initiator group configuration')
    p.set_defaults(func=rpc.iscsi.get_initiator_groups)

    p = subparsers.add_parser('get_target_nodes', help='Display target nodes')
    p.set_defaults(func=rpc.iscsi.get_target_nodes)

    p = subparsers.add_parser('construct_target_node',
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
    p.add_argument('chap_disabled', help="""CHAP authentication should be disabled for this target node.
    *** Mutually exclusive with chap_required ***""", type=int)
    p.add_argument('chap_required', help="""CHAP authentication should be required for this target node.
    *** Mutually exclusive with chap_disabled ***""", type=int)
    p.add_argument(
        'chap_mutual', help='CHAP authentication should be mutual/bidirectional.', type=int)
    p.add_argument('chap_auth_group', help="""Authentication group ID for this target node.
    *** Authentication group must be precreated ***""", type=int)
    p.add_argument('-H', dest='header_digest',
                   help='Header Digest should be required for this target node.', type=int, required=False)
    p.add_argument('-D', dest='data_digest',
                   help='Data Digest should be required for this target node.', type=int, required=False)
    p.set_defaults(func=rpc.iscsi.construct_target_node)

    p = subparsers.add_parser('target_node_add_lun', help='Add LUN to the target node')
    p.add_argument('name', help='Target node name (ASCII)')
    p.add_argument('bdev_name', help="""bdev name enclosed in quotes.
    *** bdev name cannot contain space or colon characters ***""")
    p.add_argument('-i', dest='lun_id', help="""LUN ID (integer >= 0)
    *** If LUN ID is omitted or -1, the lowest free one is assigned ***""", type=int, required=False)
    p.set_defaults(func=rpc.iscsi.target_node_add_lun)

    p = subparsers.add_parser('add_pg_ig_maps', help='Add PG-IG maps to the target node')
    p.add_argument('name', help='Target node name (ASCII)')
    p.add_argument('pg_ig_mappings', help="""List of (Portal_Group_Tag:Initiator_Group_Tag) mappings
    Whitespace separated, quoted, mapping defined with colon
    separated list of "tags" (int > 0)
    Example: '1:1 2:2 2:1'
    *** The Portal/Initiator Groups must be precreated ***""")
    p.set_defaults(func=rpc.iscsi.add_pg_ig_maps)

    p = subparsers.add_parser('delete_pg_ig_maps', help='Delete PG-IG maps from the target node')
    p.add_argument('name', help='Target node name (ASCII)')
    p.add_argument('pg_ig_mappings', help="""List of (Portal_Group_Tag:Initiator_Group_Tag) mappings
    Whitespace separated, quoted, mapping defined with colon
    separated list of "tags" (int > 0)
    Example: '1:1 2:2 2:1'
    *** The Portal/Initiator Groups must be precreated ***""")
    p.set_defaults(func=rpc.iscsi.delete_pg_ig_maps)

    p = subparsers.add_parser('add_portal_group', help='Add a portal group')
    p.add_argument(
        'tag', help='Portal group tag (unique, integer > 0)', type=int)
    p.add_argument('portal_list', nargs=argparse.REMAINDER, help="""List of portals in 'host:port@cpumask' format, separated by whitespace
    (cpumask is optional and can be skipped)
    Example: '192.168.100.100:3260' '192.168.100.100:3261' '192.168.100.100:3262@0x1""")
    p.set_defaults(func=rpc.iscsi.add_portal_group)

    p = subparsers.add_parser('add_initiator_group',
                              help='Add an initiator group')
    p.add_argument(
        'tag', help='Initiator group tag (unique, integer > 0)', type=int)
    p.add_argument('initiator_list', help="""Whitespace-separated list of initiator hostnames or IP addresses,
    enclosed in quotes.  Example: 'ANY' or '127.0.0.1 192.168.200.100'""")
    p.add_argument('netmask_list', help="""Whitespace-separated list of initiator netmasks enclosed in quotes.
    Example: '255.255.0.0 255.248.0.0' etc""")
    p.set_defaults(func=rpc.iscsi.add_initiator_group)

    p = subparsers.add_parser('delete_target_node',
                              help='Delete a target node')
    p.add_argument('target_node_name',
                   help='Target node name to be deleted. Example: iqn.2016-06.io.spdk:disk1.')
    p.set_defaults(func=rpc.iscsi.delete_target_node)

    p = subparsers.add_parser('delete_portal_group',
                              help='Delete a portal group')
    p.add_argument(
        'tag', help='Portal group tag (unique, integer > 0)', type=int)
    p.set_defaults(func=rpc.iscsi.delete_portal_group)

    p = subparsers.add_parser('delete_initiator_group',
                              help='Delete an initiator group')
    p.add_argument(
        'tag', help='Initiator group tag (unique, integer > 0)', type=int)
    p.set_defaults(func=rpc.iscsi.delete_initiator_group)

    p = subparsers.add_parser('get_iscsi_connections',
                              help='Display iSCSI connections')
    p.set_defaults(func=rpc.iscsi.get_iscsi_connections)

    p = subparsers.add_parser('get_iscsi_global_params', help='Display iSCSI global parameters')
    p.set_defaults(func=rpc.iscsi.get_iscsi_global_params)

    p = subparsers.add_parser('get_scsi_devices', help='Display SCSI devices')
    p.set_defaults(func=rpc.iscsi.get_scsi_devices)

    # log
    p = subparsers.add_parser('set_trace_flag', help='set trace flag')
    p.add_argument(
        'flag', help='trace mask we want to set. (for example "debug").')
    p.set_defaults(func=rpc.log.set_trace_flag)

    p = subparsers.add_parser('clear_trace_flag', help='clear trace flag')
    p.add_argument(
        'flag', help='trace mask we want to clear. (for example "debug").')
    p.set_defaults(func=rpc.log.clear_trace_flag)

    p = subparsers.add_parser('get_trace_flags', help='get trace flags')
    p.set_defaults(func=rpc.log.get_trace_flags)

    p = subparsers.add_parser('set_log_level', help='set log level')
    p.add_argument('level', help='log level we want to set. (for example "DEBUG").')
    p.set_defaults(func=rpc.log.set_log_level)

    p = subparsers.add_parser('get_log_level', help='get log level')
    p.set_defaults(func=rpc.log.get_log_level)

    p = subparsers.add_parser('set_log_print_level', help='set log print level')
    p.add_argument('level', help='log print level we want to set. (for example "DEBUG").')
    p.set_defaults(func=rpc.log.set_log_print_level)

    p = subparsers.add_parser('get_log_print_level', help='get log print level')
    p.set_defaults(func=rpc.log.get_log_print_level)

    # lvol
    p = subparsers.add_parser('construct_lvol_store', help='Add logical volume store on base bdev')
    p.add_argument('bdev_name', help='base bdev name')
    p.add_argument('lvs_name', help='name for lvol store')
    p.add_argument('-c', '--cluster-sz', help='size of cluster (in bytes)', type=int, required=False)
    p.set_defaults(func=rpc.lvol.construct_lvol_store)

    p = subparsers.add_parser('construct_lvol_bdev', help='Add a bdev with an logical volume backend')
    p.add_argument('-u', '--uuid', help='lvol store UUID', required=False)
    p.add_argument('-l', '--lvs_name', help='lvol store name', required=False)
    p.add_argument('lvol_name', help='name for this lvol')
    p.add_argument('size', help='size in MiB for this bdev', type=int)
    p.set_defaults(func=rpc.lvol.construct_lvol_bdev)

    # Logical volume resize feature is disabled, as it is currently work in progress
    # p = subparsers.add_parser('resize_lvol_bdev', help='Resize existing lvol bdev')
    # p.add_argument('name', help='lvol bdev name')
    # p.add_argument('size', help='new size in MiB for this bdev', type=int)
    # p.set_defaults(func=rpc.lvol.resize_lvol_bdev)

    p = subparsers.add_parser('destroy_lvol_store', help='Destroy an logical volume store')
    p.add_argument('-u', '--uuid', help='lvol store UUID', required=False)
    p.add_argument('-l', '--lvs_name', help='lvol store name', required=False)
    p.set_defaults(func=rpc.lvol.destroy_lvol_store)

    p = subparsers.add_parser('get_lvol_stores', help='Display current logical volume store list')
    p.set_defaults(func=rpc.lvol.get_lvol_stores)

    # nbd
    p = subparsers.add_parser('start_nbd_disk', help='Export a bdev as a nbd disk')
    p.add_argument('bdev_name', help='Blockdev name to be exported. Example: Malloc0.')
    p.add_argument('nbd_device', help='Nbd device name to be assigned. Example: /dev/nbd0.')
    p.set_defaults(func=rpc.nbd.start_nbd_disk)

    p = subparsers.add_parser('stop_nbd_disk', help='Stop a nbd disk')
    p.add_argument('nbd_device', help='Nbd device name to be stopped. Example: /dev/nbd0.')
    p.set_defaults(func=rpc.nbd.stop_nbd_disk)

    p = subparsers.add_parser('get_nbd_disks', help='Display full or specified nbd device list')
    p.add_argument('-n', '--nbd_device', help="Path of the nbd device. Example: /dev/nbd0", required=False)
    p.set_defaults(func=rpc.nbd.get_nbd_disks)

    # net
    p = subparsers.add_parser('add_ip_address', help='Add IP address')
    p.add_argument('ifc_index', help='ifc index of the nic device.', type=int)
    p.add_argument('ip_addr', help='ip address will be added.')
    p.set_defaults(func=rpc.net.add_ip_address)

    p = subparsers.add_parser('delete_ip_address', help='Delete IP address')
    p.add_argument('ifc_index', help='ifc index of the nic device.', type=int)
    p.add_argument('ip_addr', help='ip address will be deleted.')
    p.set_defaults(func=rpc.net.delete_ip_address)

    p = subparsers.add_parser(
        'get_interfaces', help='Display current interface list')
    p.set_defaults(func=rpc.net.get_interfaces)

    # NVMe-oF
    p = subparsers.add_parser('get_nvmf_subsystems',
                              help='Display nvmf subsystems')
    p.set_defaults(func=rpc.nvmf.get_nvmf_subsystems)

    p = subparsers.add_parser('construct_nvmf_subsystem', help='Add a nvmf subsystem')
    p.add_argument('nqn', help='Target nqn(ASCII)')
    p.add_argument('listen', help="""comma-separated list of Listen <trtype:transport_name traddr:address trsvcid:port_id> pairs enclosed
    in quotes.  Format:  'trtype:transport0 traddr:traddr0 trsvcid:trsvcid0,trtype:transport1 traddr:traddr1 trsvcid:trsvcid1' etc
    Example: 'trtype:RDMA traddr:192.168.100.8 trsvcid:4420,trtype:RDMA traddr:192.168.100.9 trsvcid:4420'""")
    p.add_argument('hosts', help="""Whitespace-separated list of host nqn list.
    Format:  'nqn1 nqn2' etc
    Example: 'nqn.2016-06.io.spdk:init nqn.2016-07.io.spdk:init'""")
    p.add_argument("-a", "--allow-any-host", action='store_true', help="Allow any host to connect (don't enforce host NQN whitelist)")
    p.add_argument("-s", "--serial_number", help="""
    Format:  'sn' etc
    Example: 'SPDK00000000000001'""", default='0000:00:01.0')
    p.add_argument("-n", "--namespaces", help="""Whitespace-separated list of namespaces
    Format:  'bdev_name1[:nsid1] bdev_name2[:nsid2] bdev_name3[:nsid3]' etc
    Example: '1:Malloc0 2:Malloc1 3:Malloc2'
    *** The devices must pre-exist ***""")
    p.set_defaults(func=rpc.nvmf.construct_nvmf_subsystem)

    p = subparsers.add_parser('delete_nvmf_subsystem',
                              help='Delete a nvmf subsystem')
    p.add_argument('subsystem_nqn',
                   help='subsystem nqn to be deleted. Example: nqn.2016-06.io.spdk:cnode1.')
    p.set_defaults(func=rpc.nvmf.delete_nvmf_subsystem)

    # pmem
    p = subparsers.add_parser('create_pmem_pool', help='Create pmem pool')
    p.add_argument('pmem_file', help='Path to pmemblk pool file')
    p.add_argument('total_size', help='Size of malloc bdev in MB (int > 0)', type=int)
    p.add_argument('block_size', help='Block size for this pmem pool', type=int)
    p.set_defaults(func=rpc.pmem.create_pmem_pool)

    p = subparsers.add_parser('pmem_pool_info', help='Display pmem pool info and check consistency')
    p.add_argument('pmem_file', help='Path to pmemblk pool file')
    p.set_defaults(func=rpc.pmem.pmem_pool_info)

    p = subparsers.add_parser('delete_pmem_pool', help='Delete pmem pool')
    p.add_argument('pmem_file', help='Path to pmemblk pool file')
    p.set_defaults(func=rpc.pmem.delete_pmem_pool)

    # vhost
    p = subparsers.add_parser('set_vhost_controller_coalescing', help='Set vhost controller coalescing')
    p.add_argument('ctrlr', help='controller name')
    p.add_argument('delay_base_us', help='Base delay time', type=int)
    p.add_argument('iops_threshold', help='IOPS threshold when coalescing is enabled', type=int)
    p.set_defaults(func=rpc.vhost.set_vhost_controller_coalescing)

    p = subparsers.add_parser(
        'construct_vhost_scsi_controller', help='Add new vhost controller')
    p.add_argument('ctrlr', help='controller name')
    p.add_argument('--cpumask', help='cpu mask for this controller')
    p.set_defaults(func=rpc.vhost.construct_vhost_scsi_controller)

    p = subparsers.add_parser('add_vhost_scsi_lun',
                              help='Add lun to vhost controller')
    p.add_argument('ctrlr', help='conntroller name where add lun')
    p.add_argument('scsi_target_num', help='scsi_target_num', type=int)
    p.add_argument('bdev_name', help='bdev name')
    p.set_defaults(func=rpc.vhost.add_vhost_scsi_lun)

    p = subparsers.add_parser('remove_vhost_scsi_target', help='Remove target from vhost controller')
    p.add_argument('ctrlr', help='controller name to remove target from')
    p.add_argument('scsi_target_num', help='scsi_target_num', type=int)
    p.set_defaults(func=rpc.vhost.remove_vhost_scsi_target)

    p = subparsers.add_parser('construct_vhost_blk_controller', help='Add a new vhost block controller')
    p.add_argument('ctrlr', help='controller name')
    p.add_argument('dev_name', help='device name')
    p.add_argument('--cpumask', help='cpu mask for this controller')
    p.add_argument("-r", "--readonly", action='store_true', help='Set controller as read-only')
    p.set_defaults(func=rpc.vhost.construct_vhost_blk_controller)

    p = subparsers.add_parser('get_vhost_controllers', help='List vhost controllers')
    p.set_defaults(func=rpc.vhost.get_vhost_controllers)

    p = subparsers.add_parser('remove_vhost_controller', help='Remove a vhost controller')
    p.add_argument('ctrlr', help='controller name')
    p.set_defaults(func=rpc.vhost.remove_vhost_controller)

    p = subparsers.add_parser('construct_virtio_user_scsi_bdev', help="""Connect to virtio user scsi device.
    This imply scan and add bdevs offered by remote side.
    Result is array of added bdevs.""")
    p.add_argument('path', help='Path to Virtio SCSI socket')
    p.add_argument('name', help="""Use this name as base instead of 'VirtioScsiN'
    Base will be used to construct new bdev's found on target by adding 't<TARGET_ID>' sufix.""")
    p.add_argument('--vq-count', help='Number of virtual queues to be used.', type=int)
    p.add_argument('--vq-size', help='Size of each queue', type=int)
    p.set_defaults(func=rpc.vhost.construct_virtio_user_scsi_bdev)

    p = subparsers.add_parser('construct_virtio_pci_scsi_bdev', help="""Create a Virtio
    SCSI device from a virtio-pci device.""")
    p.add_argument('pci_address', help="""PCI address in domain:bus:device.function format or
    domain.bus.device.function format""")
    p.add_argument('name', help="""Name for the virtio device.
    It will be inhereted by all created bdevs, which are named n the following format: <name>t<target_id>""")
    p.set_defaults(func=rpc.vhost.construct_virtio_pci_scsi_bdev)

    p = subparsers.add_parser('remove_virtio_scsi_bdev', help="""Remove a Virtio-SCSI device
    This will delete all bdevs exposed by this device""")
    p.add_argument('name', help='Virtio device name. E.g. VirtioUser0')
    p.set_defaults(func=rpc.vhost.remove_virtio_scsi_bdev)

    args = parser.parse_args()

    args.client = rpc.client.JSONRPCClient(args.server_addr, args.port)
    args.func(args)
