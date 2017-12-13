#!/usr/bin/env python

import argparse
import json
import socket

try:
    from shlex import quote
except ImportError:
    from pipes import quote

def print_dict(d):
    print json.dumps(d, indent=2)

def print_array(a):
    print " ".join((quote(v) for v in a))

parser = argparse.ArgumentParser(description='SPDK RPC command line interface')
parser.add_argument('-s', dest='server_addr', help='RPC server address', default='/var/tmp/spdk.sock')
parser.add_argument('-p', dest='port', help='RPC port number (if server_addr is IP address)', default=5260, type=int)
parser.add_argument('-v', dest='verbose', help='Verbose mode', action='store_true')
subparsers = parser.add_subparsers(help='RPC methods')


def int_arg(arg):
    return int(arg, 0)


def jsonrpc_call(method, params={}):
    if args.server_addr.startswith('/'):
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.connect(args.server_addr)
    else:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.connect((args.server_addr, args.port))
    req = {}
    req['jsonrpc'] = '2.0'
    req['method'] = method
    req['id'] = 1
    if (params):
        req['params'] = params
    reqstr = json.dumps(req)

    if args.verbose:
        print("request:")
        print(json.dumps(req, indent=2))

    s.sendall(reqstr)
    buf = ''
    closed = False
    response = {}
    while not closed:
        newdata = s.recv(4096)
        if (newdata == b''):
            closed = True
        buf += newdata
        try:
            response = json.loads(buf)
        except ValueError:
            continue  # incomplete response; keep buffering
        break
    s.close()

    if not response:
        if method == "kill_instance":
            exit(0)
        print "Connection closed with partial response:"
        print buf
        exit(1)

    if 'error' in response:
        print "Got JSON-RPC error response"
        print "request:"
        print_dict(json.loads(reqstr))
        print "response:"
        print_dict(response['error'])
        exit(1)

    if args.verbose:
        print("response:")
        print(json.dumps(response, indent=2))

    return response['result']

def get_luns(args):
    print_dict(jsonrpc_call('get_luns'))

p = subparsers.add_parser('get_luns', help='Display active LUNs')
p.set_defaults(func=get_luns)


def get_portal_groups(args):
    print_dict(jsonrpc_call('get_portal_groups'))

p = subparsers.add_parser('get_portal_groups', help='Display current portal group configuration')
p.set_defaults(func=get_portal_groups)


def get_initiator_groups(args):
    print_dict(jsonrpc_call('get_initiator_groups'))

p = subparsers.add_parser('get_initiator_groups', help='Display current initiator group configuration')
p.set_defaults(func=get_initiator_groups)


def get_target_nodes(args):
    print_dict(jsonrpc_call('get_target_nodes'))

p = subparsers.add_parser('get_target_nodes', help='Display target nodes')
p.set_defaults(func=get_target_nodes)


def construct_target_node(args):
    lun_name_id_dict = dict(u.split(":")
                            for u in args.lun_name_id_pairs.strip().split(" "))
    lun_names = lun_name_id_dict.keys()
    lun_ids = list(map(int, lun_name_id_dict.values()))

    pg_tags = []
    ig_tags = []
    for u in args.pg_ig_mappings.strip().split(" "):
        pg, ig = u.split(":")
        pg_tags.append(int(pg))
        ig_tags.append(int(ig))

    params = {
        'name': args.name,
        'alias_name': args.alias_name,
        'pg_tags': pg_tags,
        'ig_tags': ig_tags,
        'lun_names': lun_names,
        'lun_ids': lun_ids,
        'queue_depth': args.queue_depth,
        'chap_disabled': args.chap_disabled,
        'chap_required': args.chap_required,
        'chap_mutual': args.chap_mutual,
        'chap_auth_group': args.chap_auth_group,
    }
    jsonrpc_call('construct_target_node', params)

p = subparsers.add_parser('construct_target_node', help='Add a target node')
p.add_argument('name', help='Target node name (ASCII)')
p.add_argument('alias_name', help='Target node alias name (ASCII)')
p.add_argument('lun_name_id_pairs', help="""Whitespace-separated list of LUN <name:id> pairs enclosed
in quotes.  Format:  'lun_name0:id0 lun_name1:id1' etc
Example: 'Malloc0:0 Malloc1:1 Malloc5:2'
*** The LUNs must pre-exist ***
*** LUN0 (id = 0) is required ***
*** LUN names cannot contain space or colon characters ***""")
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
p.add_argument('chap_mutual', help='CHAP authentication should be mutual/bidirectional.', type=int)
p.add_argument('chap_auth_group', help="""Authentication group ID for this target node.
*** Authentication group must be precreated ***""", type=int)
p.set_defaults(func=construct_target_node)


def add_pg_ig_maps(args):
    pg_tags = []
    ig_tags = []
    for u in args.pg_ig_mappings.strip().split(" "):
        pg, ig = u.split(":")
        pg_tags.append(int(pg))
        ig_tags.append(int(ig))

    params = {
        'name': args.name,
        'pg_tags': pg_tags,
        'ig_tags': ig_tags,
    }
    jsonrpc_call('add_pg_ig_maps', params)

p = subparsers.add_parser('add_pg_ig_maps', help='Add PG-IG maps to the target node')
p.add_argument('name', help='Target node name (ASCII)')
p.add_argument('pg_ig_mappings', help="""List of (Portal_Group_Tag:Initiator_Group_Tag) mappings
Whitespace separated, quoted, mapping defined with colon
separated list of "tags" (int > 0)
Example: '1:1 2:2 2:1'
*** The Portal/Initiator Groups must be precreated ***""")
p.set_defaults(func=add_pg_ig_maps)


def delete_pg_ig_maps(args):
    pg_tags = []
    ig_tags = []
    for u in args.pg_ig_mappings.strip().split(" "):
        pg, ig = u.split(":")
        pg_tags.append(int(pg))
        ig_tags.append(int(ig))

    params = {
        'name': args.name,
        'pg_tags': pg_tags,
        'ig_tags': ig_tags,
    }
    jsonrpc_call('delete_pg_ig_maps', params)

p = subparsers.add_parser('delete_pg_ig_maps', help='Delete PG-IG maps from the target node')
p.add_argument('name', help='Target node name (ASCII)')
p.add_argument('pg_ig_mappings', help="""List of (Portal_Group_Tag:Initiator_Group_Tag) mappings
Whitespace separated, quoted, mapping defined with colon
separated list of "tags" (int > 0)
Example: '1:1 2:2 2:1'
*** The Portal/Initiator Groups must be precreated ***""")
p.set_defaults(func=delete_pg_ig_maps)


def construct_malloc_bdev(args):
    num_blocks = (args.total_size * 1024 * 1024) / args.block_size
    params = {'num_blocks': num_blocks, 'block_size': args.block_size}
    if args.name:
        params['name'] = args.name
    print_array(jsonrpc_call('construct_malloc_bdev', params))

p = subparsers.add_parser('construct_malloc_bdev', help='Add a bdev with malloc backend')
p.add_argument('-b', '--name', help="Name of the bdev")
p.add_argument('total_size', help='Size of malloc bdev in MB (int > 0)', type=int)
p.add_argument('block_size', help='Block size for this bdev', type=int)
p.set_defaults(func=construct_malloc_bdev)


def create_pmem_pool(args):
    num_blocks = (args.total_size * 1024 * 1024) / args.block_size
    params = {'pmem_file': args.pmem_file,
              'num_blocks': num_blocks,
              'block_size': args.block_size}
    jsonrpc_call('create_pmem_pool', params)

p = subparsers.add_parser('create_pmem_pool', help='Create pmem pool')
p.add_argument('pmem_file', help='Path to pmemblk pool file')
p.add_argument('total_size', help='Size of malloc bdev in MB (int > 0)', type=int)
p.add_argument('block_size', help='Block size for this pmem pool', type=int)
p.set_defaults(func=create_pmem_pool)


def pmem_pool_info(args):
    params = {'pmem_file': args.pmem_file}
    print_dict(jsonrpc_call('pmem_pool_info', params))

p = subparsers.add_parser('pmem_pool_info', help='Display pmem pool info and check consistency')
p.add_argument('pmem_file', help='Path to pmemblk pool file')
p.set_defaults(func=pmem_pool_info)


def delete_pmem_pool(args):
    params = {'pmem_file': args.pmem_file}
    jsonrpc_call('delete_pmem_pool', params)

p = subparsers.add_parser('delete_pmem_pool', help='Delete pmem pool')
p.add_argument('pmem_file', help='Path to pmemblk pool file')
p.set_defaults(func=delete_pmem_pool)


def construct_pmem_bdev(args):
    params = {
        'pmem_file': args.pmem_file,
        'name': args.name
    }
    print_array(jsonrpc_call('construct_pmem_bdev', params))

p = subparsers.add_parser('construct_pmem_bdev', help='Add a bdev with pmem backend')
p.add_argument('pmem_file', help='Path to pmemblk pool file')
p.add_argument('-n', '--name', help='Block device name', required=True)
p.set_defaults(func=construct_pmem_bdev)

def construct_null_bdev(args):
    num_blocks = (args.total_size * 1024 * 1024) / args.block_size
    params = {'name': args.name, 'num_blocks': num_blocks, 'block_size': args.block_size}
    print_array(jsonrpc_call('construct_null_bdev', params))

p = subparsers.add_parser('construct_null_bdev', help='Add a bdev with null backend')
p.add_argument('name', help='Block device name')
p.add_argument('total_size', help='Size of null bdev in MB (int > 0)', type=int)
p.add_argument('block_size', help='Block size for this bdev', type=int)
p.set_defaults(func=construct_null_bdev)


def construct_aio_bdev(args):
    params = {'name': args.name,
              'filename': args.filename}

    if args.block_size:
        params['block_size'] = args.block_size

    print_array(jsonrpc_call('construct_aio_bdev', params))

p = subparsers.add_parser('construct_aio_bdev', help='Add a bdev with aio backend')
p.add_argument('filename', help='Path to device or file (ex: /dev/sda)')
p.add_argument('name', help='Block device name')
p.add_argument('block_size', help='Block size for this bdev', type=int, default=argparse.SUPPRESS)
p.set_defaults(func=construct_aio_bdev)

def construct_nvme_bdev(args):
    params = {'name': args.name,
              'trtype': args.trtype,
              'traddr': args.traddr}

    if args.adrfam:
        params['adrfam'] = args.adrfam

    if args.trsvcid:
        params['trsvcid'] = args.trsvcid

    if args.subnqn:
        params['subnqn'] = args.subnqn

    jsonrpc_call('construct_nvme_bdev', params)

p = subparsers.add_parser('construct_nvme_bdev', help='Add bdev with nvme backend')
p.add_argument('-b', '--name', help="Name of the bdev", required=True)
p.add_argument('-t', '--trtype', help='NVMe-oF target trtype: e.g., rdma, pcie', required=True)
p.add_argument('-a', '--traddr', help='NVMe-oF target address: e.g., an ip address or BDF', required=True)
p.add_argument('-f', '--adrfam', help='NVMe-oF target adrfam: e.g., ipv4, ipv6, ib, fc, intra_host')
p.add_argument('-s', '--trsvcid', help='NVMe-oF target trsvcid: e.g., a port number')
p.add_argument('-n', '--subnqn', help='NVMe-oF target subnqn')
p.set_defaults(func=construct_nvme_bdev)

def construct_rbd_bdev(args):
    params = {
        'pool_name': args.pool_name,
        'rbd_name': args.rbd_name,
        'block_size': args.block_size,
    }
    print_array(jsonrpc_call('construct_rbd_bdev', params))

p = subparsers.add_parser('construct_rbd_bdev', help='Add a bdev with ceph rbd backend')
p.add_argument('pool_name', help='rbd pool name')
p.add_argument('rbd_name', help='rbd image name')
p.add_argument('block_size', help='rbd block size', type=int)
p.set_defaults(func=construct_rbd_bdev)

def construct_error_bdev(args):
    params = {'base_name': args.base_name}
    jsonrpc_call('construct_error_bdev', params)
p = subparsers.add_parser('construct_error_bdev', help='Add bdev with error injection backend')
p.add_argument('base_name', help='base bdev name')
p.set_defaults(func=construct_error_bdev)


def construct_lvol_store(args):
    params = {'bdev_name': args.bdev_name, 'lvs_name': args.lvs_name}

    if args.cluster_sz:
        params['cluster_sz'] = args.cluster_sz

    print_array(jsonrpc_call('construct_lvol_store', params))
p = subparsers.add_parser('construct_lvol_store', help='Add logical volume store on base bdev')
p.add_argument('bdev_name', help='base bdev name')
p.add_argument('lvs_name', help='name for lvol store')
p.add_argument('-c', '--cluster-sz', help='size of cluster (in bytes)', type=int, required=False)
p.set_defaults(func=construct_lvol_store)


def construct_lvol_bdev(args):
    num_bytes = (args.size * 1024 * 1024)
    params = {'lvol_name': args.lvol_name, 'size': num_bytes}
    if (args.uuid and args.lvs_name) or (not args.uuid and not args.lvs_name):
        print("You need to specify either uuid or name of lvolstore")
    else:
        if args.uuid:
            params['uuid'] = args.uuid
        if args.lvs_name:
            params['lvs_name'] = args.lvs_name
        print_array(jsonrpc_call('construct_lvol_bdev', params))
p = subparsers.add_parser('construct_lvol_bdev', help='Add a bdev with an logical volume backend')
p.add_argument('-u', '--uuid', help='lvol store UUID', required=False)
p.add_argument('-l', '--lvs_name', help='lvol store name', required=False)
p.add_argument('lvol_name', help='name for this lvol')
p.add_argument('size', help='size in MiB for this bdev', type=int)
p.set_defaults(func=construct_lvol_bdev)

# Logical volume resize feature is disabled, as it is currently work in progress
#
# def resize_lvol_bdev(args):
#     params = {
#         'name': args.name,
#         'size': args.size,
#     }
#     jsonrpc_call('resize_lvol_bdev', params)
# p = subparsers.add_parser('resize_lvol_bdev', help='Resize existing lvol bdev')
# p.add_argument('name', help='lvol bdev name')
# p.add_argument('size', help='new size in MiB for this bdev', type=int)
# p.set_defaults(func=resize_lvol_bdev)


def destroy_lvol_store(args):
    params = {}
    if (args.uuid and args.lvs_name) or (not args.uuid and not args.lvs_name):
        print("You need to specify either uuid or name of lvolstore")
    else:
        if args.uuid:
            params['uuid'] = args.uuid
        if args.lvs_name:
            params['lvs_name'] = args.lvs_name
        jsonrpc_call('destroy_lvol_store', params)
p = subparsers.add_parser('destroy_lvol_store', help='Destroy an logical volume store')
p.add_argument('-u', '--uuid', help='lvol store UUID', required=False)
p.add_argument('-l', '--lvs_name', help='lvol store name', required=False)
p.set_defaults(func=destroy_lvol_store)


def get_lvol_stores(args):
    print_dict(jsonrpc_call('get_lvol_stores'))

p = subparsers.add_parser('get_lvol_stores', help='Display current logical volume store list')
p.set_defaults(func=get_lvol_stores)


def set_trace_flag(args):
    params = {'flag': args.flag}
    jsonrpc_call('set_trace_flag', params)

p = subparsers.add_parser('set_trace_flag', help='set trace flag')
p.add_argument('flag', help='trace mask we want to set. (for example "debug").')
p.set_defaults(func=set_trace_flag)


def clear_trace_flag(args):
    params = {'flag': args.flag}
    jsonrpc_call('clear_trace_flag', params)

p = subparsers.add_parser('clear_trace_flag', help='clear trace flag')
p.add_argument('flag', help='trace mask we want to clear. (for example "debug").')
p.set_defaults(func=clear_trace_flag)


def get_trace_flags(args):
    print_dict(jsonrpc_call('get_trace_flags'))

p = subparsers.add_parser('get_trace_flags', help='get trace flags')
p.set_defaults(func=get_trace_flags)

def set_log_level(args):
    params = {'level': args.level}
    jsonrpc_call('set_log_level', params)

p = subparsers.add_parser('set_log_level', help='set log level')
p.add_argument('level', help='log level we want to set. (for example "DEBUG").')
p.set_defaults(func=set_log_level)

def get_log_level(args):
    print_dict(jsonrpc_call('get_log_level'))

p = subparsers.add_parser('get_log_level', help='get log level')
p.set_defaults(func=get_log_level)

def set_log_print_level(args):
    params = {'level': args.level}
    jsonrpc_call('set_log_print_level', params)

p = subparsers.add_parser('set_log_print_level', help='set log print level')
p.add_argument('level', help='log print level we want to set. (for example "DEBUG").')
p.set_defaults(func=set_log_print_level)

def get_log_print_level(args):
    print_dict(jsonrpc_call('get_log_print_level'))

p = subparsers.add_parser('get_log_print_level', help='get log print level')
p.set_defaults(func=get_log_print_level)

def add_portal_group(args):
    # parse out portal list host1:port1 host2:port2
    portals = []
    for p in args.portal_list:
        host_port = p.split(':')
        portals.append({'host': host_port[0], 'port': host_port[1]})

    params = {'tag': args.tag, 'portals': portals}
    jsonrpc_call('add_portal_group', params)

p = subparsers.add_parser('add_portal_group', help='Add a portal group')
p.add_argument('tag', help='Portal group tag (unique, integer > 0)', type=int)
p.add_argument('portal_list', nargs=argparse.REMAINDER, help="""List of portals in 'host:port' format, separated by whitespace
Example: '192.168.100.100:3260' '192.168.100.100:3261'""")
p.set_defaults(func=add_portal_group)


def add_initiator_group(args):
    initiators = []
    netmasks = []
    for i in args.initiator_list.strip().split(' '):
        initiators.append(i)
    for n in args.netmask_list.strip().split(' '):
        netmasks.append(n)

    params = {'tag': args.tag, 'initiators': initiators, 'netmasks': netmasks}
    jsonrpc_call('add_initiator_group', params)


p = subparsers.add_parser('add_initiator_group', help='Add an initiator group')
p.add_argument('tag', help='Initiator group tag (unique, integer > 0)', type=int)
p.add_argument('initiator_list', help="""Whitespace-separated list of initiator hostnames or IP addresses,
enclosed in quotes.  Example: 'ANY' or '127.0.0.1 192.168.200.100'""")
p.add_argument('netmask_list', help="""Whitespace-separated list of initiator netmasks enclosed in quotes.
Example: '255.255.0.0 255.248.0.0' etc""")
p.set_defaults(func=add_initiator_group)


def delete_target_node(args):
    params = {'name': args.target_node_name}
    jsonrpc_call('delete_target_node', params)

p = subparsers.add_parser('delete_target_node', help='Delete a target node')
p.add_argument('target_node_name', help='Target node name to be deleted. Example: iqn.2016-06.io.spdk:disk1.')
p.set_defaults(func=delete_target_node)


def delete_portal_group(args):
    params = {'tag': args.tag}
    jsonrpc_call('delete_portal_group', params)

p = subparsers.add_parser('delete_portal_group', help='Delete a portal group')
p.add_argument('tag', help='Portal group tag (unique, integer > 0)', type=int)
p.set_defaults(func=delete_portal_group)


def delete_initiator_group(args):
    params = {'tag': args.tag}
    jsonrpc_call('delete_initiator_group', params)

p = subparsers.add_parser('delete_initiator_group', help='Delete an initiator group')
p.add_argument('tag', help='Initiator group tag (unique, integer > 0)', type=int)
p.set_defaults(func=delete_initiator_group)


def get_iscsi_connections(args):
    print_dict(jsonrpc_call('get_iscsi_connections'))

p = subparsers.add_parser('get_iscsi_connections', help='Display iSCSI connections')
p.set_defaults(func=get_iscsi_connections)


def get_iscsi_global_params(args):
    print_dict(jsonrpc_call('get_iscsi_global_params'))

p = subparsers.add_parser('get_iscsi_global_params', help='Display iSCSI global parameters')
p.set_defaults(func=get_iscsi_global_params)


def get_scsi_devices(args):
    print_dict(jsonrpc_call('get_scsi_devices'))

p = subparsers.add_parser('get_scsi_devices', help='Display SCSI devices')
p.set_defaults(func=get_scsi_devices)


def add_ip_address(args):
    params = {'ifc_index': args.ifc_index, 'ip_address': args.ip_addr}
    jsonrpc_call('add_ip_address', params)

p = subparsers.add_parser('add_ip_address', help='Add IP address')
p.add_argument('ifc_index', help='ifc index of the nic device.', type=int)
p.add_argument('ip_addr', help='ip address will be added.')
p.set_defaults(func=add_ip_address)


def delete_ip_address(args):
    params = {'ifc_index': args.ifc_index, 'ip_address': args.ip_addr}
    jsonrpc_call('delete_ip_address', params)

p = subparsers.add_parser('delete_ip_address', help='Delete IP address')
p.add_argument('ifc_index', help='ifc index of the nic device.', type=int)
p.add_argument('ip_addr', help='ip address will be deleted.')
p.set_defaults(func=delete_ip_address)


def get_interfaces(args):
    print_dict(jsonrpc_call('get_interfaces'))

p = subparsers.add_parser('get_interfaces', help='Display current interface list')
p.set_defaults(func=get_interfaces)

def apply_firmware(args):

    params = {
        'filename': args.filename,
        'bdev_name': args.bdev_name,
    }

    print_dict(jsonrpc_call('apply_nvme_firmware', params))

p = subparsers.add_parser('apply_firmware', help='Download and commit firmware to NVMe device')
p.add_argument('filename', help='filename of the firmware to download')
p.add_argument('bdev_name', help='name of the NVMe device')
p.set_defaults(func=apply_firmware)

def get_bdevs(args):
    params = {}
    if args.name:
        params['name'] = args.name
    print_dict(jsonrpc_call('get_bdevs', params))

p = subparsers.add_parser('get_bdevs', help='Display current blockdev list or required blockdev')
p.add_argument('-b', '--name', help="Name of the Blockdev. Example: Nvme0n1", required=False)
p.set_defaults(func=get_bdevs)


def delete_bdev(args):
    params = {'name': args.bdev_name}
    jsonrpc_call('delete_bdev', params)

p = subparsers.add_parser('delete_bdev', help='Delete a blockdev')
p.add_argument('bdev_name', help='Blockdev name to be deleted. Example: Malloc0.')
p.set_defaults(func=delete_bdev)

def start_nbd_disk(args):
    params = {
        'bdev_name': args.bdev_name,
        'nbd_device': args.nbd_device
    }
    jsonrpc_call('start_nbd_disk', params)

p = subparsers.add_parser('start_nbd_disk', help='Export a bdev as a nbd disk')
p.add_argument('bdev_name', help='Blockdev name to be exported. Example: Malloc0.')
p.add_argument('nbd_device', help='Nbd device name to be assigned. Example: /dev/nbd0.')
p.set_defaults(func=start_nbd_disk)

def stop_nbd_disk(args):
    params = {'nbd_device': args.nbd_device}
    jsonrpc_call('stop_nbd_disk', params)

p = subparsers.add_parser('stop_nbd_disk', help='Stop a nbd disk')
p.add_argument('nbd_device', help='Nbd device name to be stopped. Example: /dev/nbd0.')
p.set_defaults(func=stop_nbd_disk)

def get_nbd_disks(args):
    params = {}
    if args.nbd_device:
        params['nbd_device'] = args.nbd_device
    print_dict(jsonrpc_call('get_nbd_disks', params))

p = subparsers.add_parser('get_nbd_disks', help='Display full or specified nbd device list')
p.add_argument('-n', '--nbd_device', help="Path of the nbd device. Example: /dev/nbd0", required=False)
p.set_defaults(func=get_nbd_disks)

def get_nvmf_subsystems(args):
    print_dict(jsonrpc_call('get_nvmf_subsystems'))

p = subparsers.add_parser('get_nvmf_subsystems', help='Display nvmf subsystems')
p.set_defaults(func=get_nvmf_subsystems)

def construct_nvmf_subsystem(args):
    listen_addresses = [dict(u.split(":") for u in a.split(" ")) for a in args.listen.split(",")]

    params = {
        'core': args.core,
        'nqn': args.nqn,
        'listen_addresses': listen_addresses,
        'serial_number': args.serial_number,
    }

    if args.hosts:
        hosts = []
        for u in args.hosts.strip().split(" "):
            hosts.append(u)
        params['hosts'] = hosts

    if args.allow_any_host:
        params['allow_any_host'] = True

    if args.namespaces:
        namespaces = []
        for u in args.namespaces.strip().split(" "):
            bdev_name = u
            nsid = 0
            if ':' in u:
                (bdev_name, nsid) = u.split(":")

            ns_params = {'bdev_name': bdev_name}

            nsid = int(nsid)
            if nsid != 0:
                ns_params['nsid'] = nsid

            namespaces.append(ns_params)
        params['namespaces'] = namespaces

    jsonrpc_call('construct_nvmf_subsystem', params)

p = subparsers.add_parser('construct_nvmf_subsystem', help='Add a nvmf subsystem')
p.add_argument("-c", "--core", help='The core Nvmf target run on', type=int, default=-1)
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
p.set_defaults(func=construct_nvmf_subsystem)

def delete_nvmf_subsystem(args):
    params = {'nqn': args.subsystem_nqn}
    jsonrpc_call('delete_nvmf_subsystem', params)

p = subparsers.add_parser('delete_nvmf_subsystem', help='Delete a nvmf subsystem')
p.add_argument('subsystem_nqn', help='subsystem nqn to be deleted. Example: nqn.2016-06.io.spdk:cnode1.')
p.set_defaults(func=delete_nvmf_subsystem)

def bdev_inject_error(args):
    params = {
        'name': args.name,
        'io_type': args.io_type,
        'error_type': args.error_type,
        'num': args.num,
    }

    jsonrpc_call('bdev_inject_error', params)

p = subparsers.add_parser('bdev_inject_error', help='bdev inject error')
p.add_argument('name', help="""the name of the error injection bdev""")
p.add_argument('io_type', help="""io_type: 'clear' 'read' 'write' 'unmap' 'flush' 'all'""")
p.add_argument('error_type', help="""error_type: 'failure' 'pending'""")
p.add_argument('-n', '--num', help='the number of commands you want to fail', type=int, default=1)
p.set_defaults(func=bdev_inject_error)

def kill_instance(args):
    params = {'sig_name': args.sig_name}
    jsonrpc_call('kill_instance', params)

p = subparsers.add_parser('kill_instance', help='Send signal to instance')
p.add_argument('sig_name', help='signal will be sent to server.')
p.set_defaults(func=kill_instance)

def set_vhost_controller_coalescing(args):
    params = {
        'ctrlr': args.ctrlr,
        'delay_base_us': args.delay_base_us,
        'iops_threshold': args.iops_threshold,
    }
    jsonrpc_call('set_vhost_controller_coalescing', params)

p = subparsers.add_parser('set_vhost_controller_coalescing', help='Set vhost controller coalescing')
p.add_argument('ctrlr', help='controller name')
p.add_argument('delay_base_us', help='Base delay time', type=int)
p.add_argument('iops_threshold', help='IOPS threshold when coalescing is enabled', type=int)
p.set_defaults(func=set_vhost_controller_coalescing)

def construct_vhost_scsi_controller(args):
    params = {'ctrlr': args.ctrlr}

    if args.cpumask:
        params['cpumask'] = args.cpumask

    jsonrpc_call('construct_vhost_scsi_controller', params)

p = subparsers.add_parser('construct_vhost_scsi_controller', help='Add new vhost controller')
p.add_argument('ctrlr', help='controller name')
p.add_argument('--cpumask', help='cpu mask for this controller')
p.set_defaults(func=construct_vhost_scsi_controller)

def add_vhost_scsi_lun(args):
    params = {
        'ctrlr': args.ctrlr,
        'lun_name': args.lun_name,
        'scsi_target_num': args.scsi_target_num
    }

    jsonrpc_call('add_vhost_scsi_lun', params)

p = subparsers.add_parser('add_vhost_scsi_lun', help='Add lun to vhost controller')
p.add_argument('ctrlr', help='conntroller name where add lun')
p.add_argument('scsi_target_num', help='scsi_target_num', type=int)
p.add_argument('lun_name', help='lun name')
p.set_defaults(func=add_vhost_scsi_lun)

def remove_vhost_scsi_target(args):
    params = {
        'ctrlr': args.ctrlr,
        'scsi_target_num': args.scsi_target_num
    }
    jsonrpc_call('remove_vhost_scsi_target', params)

p = subparsers.add_parser('remove_vhost_scsi_target', help='Remove target from vhost controller')
p.add_argument('ctrlr', help='controller name to remove target from')
p.add_argument('scsi_target_num', help='scsi_target_num', type=int)
p.set_defaults(func=remove_vhost_scsi_target)

def construct_vhost_blk_controller(args):
    params = {
        'ctrlr': args.ctrlr,
        'dev_name': args.dev_name,
    }
    if args.cpumask:
        params['cpumask'] = args.cpumask
    if args.readonly:
        params['readonly'] = args.readonly
    jsonrpc_call('construct_vhost_blk_controller', params)

p = subparsers.add_parser('construct_vhost_blk_controller', help='Add a new vhost block controller')
p.add_argument('ctrlr', help='controller name')
p.add_argument('dev_name', help='device name')
p.add_argument('--cpumask', help='cpu mask for this controller')
p.add_argument("-r", "--readonly", action='store_true', help='Set controller as read-only')
p.set_defaults(func=construct_vhost_blk_controller)

def get_vhost_controllers(args):
    print_dict(jsonrpc_call('get_vhost_controllers'))

p = subparsers.add_parser('get_vhost_controllers', help='List vhost controllers')
p.set_defaults(func=get_vhost_controllers)

def remove_vhost_controller(args):
    params = {'ctrlr': args.ctrlr}
    jsonrpc_call('remove_vhost_controller', params)

p = subparsers.add_parser('remove_vhost_controller', help='Remove a vhost controller')
p.add_argument('ctrlr', help='controller name')
p.set_defaults(func=remove_vhost_controller)

def construct_virtio_user_scsi_bdev(args):
    params = {
        'path': args.path,
        'name': args.name,
    }
    if args.vq_count:
        params['vq_count'] = args.vq_count
    if args.vq_size:
        params['vq_size'] = args.vq_size
    print_dict(jsonrpc_call('construct_virtio_user_scsi_bdev', params))

p = subparsers.add_parser('construct_virtio_user_scsi_bdev', help="""Connect to virtio user scsi device.
This imply scan and add bdevs offered by remote side.
Result is array of added bdevs.""")
p.add_argument('path', help='Path to Virtio SCSI socket')
p.add_argument('name', help="""Use this name as base instead of 'VirtioScsiN'
Base will be used to construct new bdev's found on target by adding 't<TARGET_ID>' sufix.""")
p.add_argument('--vq-count', help='Number of virtual queues to be used.', type=int)
p.add_argument('--vq-size', help='Size of each queue', type=int)
p.set_defaults(func=construct_virtio_user_scsi_bdev)

def get_rpc_methods(args):
    print_dict(jsonrpc_call('get_rpc_methods'))

p = subparsers.add_parser('get_rpc_methods', help='Get list of supported RPC methods')
p.set_defaults(func=get_rpc_methods)

def context_switch_monitor(args):
    params = {}
    if args.enable:
        params['enabled'] = True
    if args.disable:
        params['enabled'] = False
    print_dict(jsonrpc_call('context_switch_monitor', params))

p = subparsers.add_parser('context_switch_monitor', help='Control whether the context switch monitor is enabled')
p.add_argument('-e', '--enable', action='store_true', help='Enable context switch monitoring')
p.add_argument('-d', '--disable', action='store_true', help='Disable context switch monitoring')
p.set_defaults(func=context_switch_monitor)

args = parser.parse_args()
args.func(args)
