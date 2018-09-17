#!/usr/bin/env python3

import os
import sys
import argparse
sys.path.append(os.path.join(os.path.dirname(__file__), "../../scripts"))
import rpc   # noqa
from rpc.client import print_dict, JSONRPCException  # noqa


def get_bdev_name_key(bdev):
    bdev_name_key = 'name'
    if 'method' in bdev and bdev['method'] == 'construct_split_vbdev':
        bdev_name_key = "base_bdev"
    return bdev_name_key


def get_bdev_name(bdev):
    bdev_name = None
    if 'params' in bdev:
        if 'name' in bdev['params']:
            bdev_name = bdev['params']['name']
        elif 'base_name' in bdev['params']:
            bdev_name = bdev['params']['base_name']
        elif 'base_bdev' in bdev['params']:
            bdev_name = bdev['params']['base_bdev']
    if 'method' in bdev and bdev['method'] == 'construct_error_bdev':
        bdev_name = "EE_%s" % bdev_name
    return bdev_name


def delete_subbdevs(args, bdev, rpc_bdevs):
    ret_value = False
    bdev_name = get_bdev_name(bdev)
    if bdev_name and 'method' in bdev:
        construct_method = bdev['method']
        if construct_method == 'construct_nvme_bdev':
            for rpc_bdev in rpc_bdevs:
                if bdev_name in rpc_bdev['name'] and rpc_bdev['product_name'] == "NVMe disk":
                    args.client.call('delete_nvme_controller', {'name': "%s" % rpc_bdev['name'].split('n')[0]})
                    ret_value = True

    return ret_value


def get_bdev_destroy_method(bdev):
    destroy_method_map = {'construct_nvme_bdev': "delete_nvme_controller",
                          'construct_malloc_bdev': "delete_malloc_bdev",
                          'construct_null_bdev': "delete_null_bdev",
                          'construct_rbd_bdev': "delete_rbd_bdev",
                          'construct_pmem_bdev': "delete_pmem_bdev",
                          'construct_aio_bdev': "delete_aio_bdev",
                          'construct_error_bdev': "delete_error_bdev",
                          'construct_split_vbdev': "destruct_split_vbdev",
                          'construct_virtio_dev': "remove_virtio_bdev",
                          'construct_crypto_bdev': "delete_crypto_bdev"
                          }
    destroy_method = None
    if 'method' in bdev:
        construct_method = bdev['method']
        if construct_method in list(destroy_method_map.keys()):
            destroy_method = destroy_method_map[construct_method]

    return destroy_method


def clear_bdev_subsystem(args, bdev_config):
    rpc_bdevs = args.client.call("get_bdevs")
    for bdev in bdev_config:
        if delete_subbdevs(args, bdev, rpc_bdevs):
            continue
        bdev_name_key = get_bdev_name_key(bdev)
        bdev_name = get_bdev_name(bdev)
        destroy_method = get_bdev_destroy_method(bdev)
        if destroy_method:
            args.client.call(destroy_method, {bdev_name_key: bdev_name})

    ''' Disable and reset hotplug '''
    rpc.bdev.set_bdev_nvme_hotplug(args.client, False)


def get_nvmf_destroy_method(nvmf):
    destroy_method_map = {'nvmf_subsystem_create': "delete_nvmf_subsystem"}
    try:
        return destroy_method_map[nvmf['method']]
    except KeyError:
        return None


def clear_nvmf_subsystem(args, nvmf_config):
    for nvmf in nvmf_config:
        destroy_method = get_nvmf_destroy_method(nvmf)
        if destroy_method:
            args.client.call(destroy_method, {'nqn': nvmf['params']['nqn']})


def get_iscsi_destroy_method(iscsi):
    destroy_method_map = {'add_portal_group': "delete_portal_group",
                          'add_initiator_group': "delete_initiator_group",
                          'construct_target_node': "delete_target_node",
                          'set_iscsi_options': None
                          }
    return destroy_method_map[iscsi['method']]


def get_iscsi_name(iscsi):
    if 'name' in iscsi['params']:
        return iscsi['params']['name']
    else:
        return iscsi['params']['tag']


def get_iscsi_name_key(iscsi):
    if iscsi['method'] == 'construct_target_node':
        return "name"
    else:
        return 'tag'


def clear_iscsi_subsystem(args, iscsi_config):
    for iscsi in iscsi_config:
        destroy_method = get_iscsi_destroy_method(iscsi)
        if destroy_method:
            args.client.call(destroy_method, {get_iscsi_name_key(iscsi): get_iscsi_name(iscsi)})


def get_nbd_destroy_method(nbd):
    destroy_method_map = {'start_nbd_disk': "stop_nbd_disk"
                          }
    return destroy_method_map[nbd['method']]


def clear_nbd_subsystem(args, nbd_config):
    for nbd in nbd_config:
        destroy_method = get_nbd_destroy_method(nbd)
        if destroy_method:
            args.client.call(destroy_method, {'nbd_device': nbd['params']['nbd_device']})


def clear_net_framework_subsystem(args, net_framework_config):
    pass


def clear_copy_subsystem(args, copy_config):
    pass


def clear_interface_subsystem(args, interface_config):
    pass


def clear_vhost_subsystem(args, vhost_config):
    for vhost in reversed(vhost_config):
        if 'method' in vhost:
            method = vhost['method']
            if method in ['add_vhost_scsi_lun']:
                args.client.call("remove_vhost_scsi_target",
                                 {"ctrlr": vhost['params']['ctrlr'],
                                  "scsi_target_num": vhost['params']['scsi_target_num']})
            elif method in ['construct_vhost_scsi_controller', 'construct_vhost_blk_controller',
                            'construct_vhost_nvme_controller']:
                args.client.call("remove_vhost_controller", {'ctrlr': vhost['params']['ctrlr']})


def call_test_cmd(func):
    def rpc_test_cmd(*args, **kwargs):
        try:
            func(*args, **kwargs)
        except JSONRPCException as ex:
            print((ex.message))
            exit(1)
    return rpc_test_cmd


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description='Clear config command')
    parser.add_argument('-s', dest='server_addr', default='/var/tmp/spdk.sock')
    parser.add_argument('-p', dest='port', default=5260, type=int)
    parser.add_argument('-t', dest='timeout', default=60.0, type=float)
    parser.add_argument('-v', dest='verbose', action='store_true')
    subparsers = parser.add_subparsers(help='RPC methods')

    @call_test_cmd
    def clear_config(args):
        for subsystem_item in reversed(args.client.call('get_subsystems')):
            args.subsystem = subsystem_item['subsystem']
            clear_subsystem(args)

    p = subparsers.add_parser('clear_config', help="""Clear configuration of all SPDK subsystems and targets using JSON RPC""")
    p.set_defaults(func=clear_config)

    @call_test_cmd
    def clear_subsystem(args):
        config = args.client.call('get_subsystem_config', {"name": args.subsystem})
        if config is None:
            return
        if args.verbose:
            print("Calling clear_%s_subsystem" % args.subsystem)
        globals()["clear_%s_subsystem" % args.subsystem](args, config)

    p = subparsers.add_parser('clear_subsystem', help="""Clear configuration of SPDK subsystem using JSON RPC""")
    p.add_argument('--subsystem', help="""Subsystem name""")
    p.set_defaults(func=clear_subsystem)

    args = parser.parse_args()

    try:
        args.client = rpc.client.JSONRPCClient(args.server_addr, args.port, args.verbose, args.timeout)
    except JSONRPCException as ex:
        print((ex.message))
        exit(1)
    args.func(args)
