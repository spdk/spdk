#!/usr/bin/env python

import argparse
import json
import socket

try:
    from shlex import quote
except ImportError:
    from pipes import quote

SPDK_JSONRPC_PORT_BASE = 5260

def print_dict(d):
    print json.dumps(d, indent=2)

def print_array(a):
    print " ".join((quote(v) for v in a))

parser = argparse.ArgumentParser(description='SPDK RPC command line interface')
parser.add_argument('-s', dest='server_addr', help='RPC server address', default='127.0.0.1')
parser.add_argument('-p', dest='instance_id', help='RPC server instance ID', default=0, type=int)
subparsers = parser.add_subparsers(help='RPC methods')


def int_arg(arg):
    return int(arg, 0)


def jsonrpc_call(method, params={}):
    if args.server_addr.startswith('/'):
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.connect("{}.{}".format(args.server_addr, args.instance_id))
    else:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.connect((args.server_addr, SPDK_JSONRPC_PORT_BASE + args.instance_id))
    req = {}
    req['jsonrpc'] = '2.0'
    req['method'] = method
    req['id'] = 1
    if (params):
        req['params'] = params
    reqstr = json.dumps(req)
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


def construct_malloc_bdev(args):
    num_blocks = (args.total_size * 1024 * 1024) / args.block_size
    params = {'num_blocks': num_blocks, 'block_size': args.block_size}
    print_array(jsonrpc_call('construct_malloc_bdev', params))

p = subparsers.add_parser('construct_malloc_bdev', help='Add a bdev with malloc backend')
p.add_argument('total_size', help='Size of malloc bdev in MB (int > 0)', type=int)
p.add_argument('block_size', help='Block size for this bdev', type=int)
p.set_defaults(func=construct_malloc_bdev)


def construct_aio_bdev(args):
    params = {'fname': args.fname}
    print_array(jsonrpc_call('construct_aio_bdev', params))

p = subparsers.add_parser('construct_aio_bdev', help='Add a bdev with aio backend')
p.add_argument('fname', help='Path to device or file (ex: /dev/sda)')
p.set_defaults(func=construct_aio_bdev)

def construct_nvme_bdev(args):
    params = {'pci_address': args.pci_address}
    print_array(jsonrpc_call('construct_nvme_bdev', params))
p = subparsers.add_parser('construct_nvme_bdev', help='Add bdev with nvme backend')
p.add_argument('pci_address', help='PCI address domain:bus:device.function')
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
enclosed in quotes.  Example: 'ALL' or '127.0.0.1 192.168.200.100'""")
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


def delete_lun(args):
    params = {'name': args.lun_name}
    jsonrpc_call('delete_lun', params)

p = subparsers.add_parser('delete_lun', help='Delete a LUN')
p.add_argument('lun_name', help='LUN name to be deleted. Example: Malloc0.')
p.set_defaults(func=delete_lun)


def get_iscsi_connections(args):
    print_dict(jsonrpc_call('get_iscsi_connections'))

p = subparsers.add_parser('get_iscsi_connections', help='Display iSCSI connections')
p.set_defaults(func=get_iscsi_connections)


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

def get_bdevs(args):
    print_dict(jsonrpc_call('get_bdevs'))

p = subparsers.add_parser('get_bdevs', help='Display current blockdev list')
p.set_defaults(func=get_bdevs)

def get_nvmf_subsystems(args):
    print_dict(jsonrpc_call('get_nvmf_subsystems'))

p = subparsers.add_parser('get_nvmf_subsystems', help='Display nvmf subsystems')
p.set_defaults(func=get_nvmf_subsystems)

def construct_nvmf_subsystem(args):
    listen_addresses = [dict(u.split(":") for u in a.split(" ")) for a in args.listen.split(",")]

    params = {
        'core': args.core,
        'mode': args.mode,
        'nqn': args.nqn,
        'listen_addresses': listen_addresses,
        'serial_number': args.serial_number,
    }

    if args.hosts:
        hosts = []
        for u in args.hosts.strip().split(" "):
            hosts.append(u)
        params['hosts'] = hosts

    if args.namespaces:
        namespaces = []
        for u in args.namespaces.strip().split(" "):
            namespaces.append(u)
        params['namespaces'] = namespaces

    if args.pci_address:
        params['pci_address'] = args.pci_address

    jsonrpc_call('construct_nvmf_subsystem', params)

p = subparsers.add_parser('construct_nvmf_subsystem', help='Add a nvmf subsystem')
p.add_argument("-c", "--core", help='The core Nvmf target run on', type=int, default=0)
p.add_argument('mode', help='Target mode: Virtual or Direct')
p.add_argument('nqn', help='Target nqn(ASCII)')
p.add_argument('listen', help="""comma-separated list of Listen <transport:transport_name traddr:address trsvcid:port_id> pairs enclosed
in quotes.  Format:  'transport:transport0 traddr:traddr0 trsvcid:trsvcid0,transport:transport1 traddr:traddr1 trsvcid:trsvcid1' etc
Example: 'transport:RDMA traddr:192.168.100.8 trsvcid:4420,transport:RDMA traddr:192.168.100.9 trsvcid:4420'""")
p.add_argument('hosts', help="""Whitespace-separated list of host nqn list.
Format:  'nqn1 nqn2' etc
Example: 'nqn.2016-06.io.spdk:init nqn.2016-07.io.spdk:init'""")
p.add_argument("-p", "--pci_address", help="""Valid if mode == Direct.
Format:  'domain:device:function' etc
Example: '0000:00:01.0'""")
p.add_argument("-s", "--serial_number", help="""Valid if mode == Virtual.
Format:  'sn' etc
Example: 'SPDK00000000000001'""", default='0000:00:01.0')
p.add_argument("-n", "--namespaces", help="""Whitespace-separated list of namespaces.
Format:  'dev1 dev2 dev3' etc
Example: 'Malloc0 Malloc1 Malloc2'
*** The devices must pre-exist ***""")
p.set_defaults(func=construct_nvmf_subsystem)

def delete_nvmf_subsystem(args):
    params = {'nqn': args.subsystem_nqn}
    jsonrpc_call('delete_nvmf_subsystem', params)

p = subparsers.add_parser('delete_nvmf_subsystem', help='Delete a nvmf subsystem')
p.add_argument('subsystem_nqn', help='subsystem nqn to be deleted. Example: nqn.2016-06.io.spdk:cnode1.')
p.set_defaults(func=delete_nvmf_subsystem)

def kill_instance(args):
    params = {'sig_name': args.sig_name}
    jsonrpc_call('kill_instance', params)

p = subparsers.add_parser('kill_instance', help='Send signal to instance')
p.add_argument('sig_name', help='signal will be sent to server.')
p.set_defaults(func=kill_instance)

def get_vhost_scsi_controllers(args):
    print_dict(jsonrpc_call('get_vhost_scsi_controllers'))

p = subparsers.add_parser('get_vhost_scsi_controllers', help='List vhost controllers')
p.set_defaults(func=get_vhost_scsi_controllers)

def construct_vhost_scsi_controller(args):
    params = {
        'ctrlr': args.ctrlr,
        'cpumask': args.cpu_mask
    }
    jsonrpc_call('construct_vhost_scsi_controller', params)

p = subparsers.add_parser('construct_vhost_scsi_controller', help='Add new vhost controller')
p.add_argument('ctrlr', help='conntroller name')
p.add_argument('cpumask', help='cpu mask for this controller')
p.set_defaults(func=construct_vhost_scsi_controller)

def add_vhost_scsi_lun(args):
    params = {
        'ctrlr': args.ctrlr,
        'scsi_dev_num': args.scsi_dev_num,
        'lun_name': args.lun_name
    }
    jsonrpc_call('add_vhost_scsi_lun', params)

p = subparsers.add_parser('add_vhost_scsi_lun', help='Add lun to vhost controller')
p.add_argument('ctrlr', help='conntroller name where add lun')
p.add_argument('scsi_dev_num', help='scsi_dev_num', type=int)
p.add_argument('lun_name', help='lun name')
p.set_defaults(func=add_vhost_scsi_lun)

args = parser.parse_args()
args.func(args)
