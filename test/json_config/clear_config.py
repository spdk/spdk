#!/usr/bin/env python3

import os
import sys
import argparse
import logging
sys.path.append(os.path.join(os.path.dirname(__file__), "../../scripts"))
import rpc   # noqa
from rpc.client import print_dict, JSONRPCException  # noqa


def get_bdev_name_key(bdev):
    bdev_name_key = 'name'
    if 'method' in bdev and bdev['method'] == 'bdev_split_create':
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
    if 'method' in bdev and bdev['method'] == 'bdev_error_create':
        bdev_name = "EE_%s" % bdev_name
    return bdev_name


def get_bdev_delete_method(bdev):
    delete_method_map = {'bdev_malloc_create': "bdev_malloc_delete",
                         'bdev_null_create': "bdev_null_delete",
                         'bdev_rbd_create': "bdev_rbd_delete",
                         'bdev_pmem_create': "bdev_pmem_delete",
                         'bdev_aio_create': "bdev_aio_delete",
                         'bdev_error_create': "bdev_error_delete",
                         'construct_split_vbdev': "destruct_split_vbdev",
                         'bdev_virtio_attach_controller': "remove_virtio_bdev",
                         'bdev_crypto_create': "bdev_crypto_delete",
                         'bdev_delay_create': "bdev_delay_delete",
                         'bdev_passthru_create': "bdev_passthru_delete",
                         'bdev_compress_create': 'bdev_compress_delete',
                         }
    destroy_method = None
    if 'method' in bdev:
        construct_method = bdev['method']
        if construct_method in list(delete_method_map.keys()):
            destroy_method = delete_method_map[construct_method]

    return destroy_method


def clear_bdev_subsystem(args, bdev_config):
    rpc_bdevs = args.client.call("bdev_get_bdevs")
    for bdev in bdev_config:
        bdev_name_key = get_bdev_name_key(bdev)
        bdev_name = get_bdev_name(bdev)
        destroy_method = get_bdev_delete_method(bdev)
        if destroy_method:
            args.client.call(destroy_method, {bdev_name_key: bdev_name})

    nvme_controllers = args.client.call("bdev_nvme_get_controllers")
    for ctrlr in nvme_controllers:
        args.client.call('bdev_nvme_detach_controller', {'name': ctrlr['name']})

    ''' Disable and reset hotplug '''
    rpc.bdev.bdev_nvme_set_hotplug(args.client, False)


def get_nvmf_destroy_method(nvmf):
    delete_method_map = {'nvmf_create_subsystem': "nvmf_delete_subsystem"}
    try:
        return delete_method_map[nvmf['method']]
    except KeyError:
        return None


def clear_nvmf_subsystem(args, nvmf_config):
    for nvmf in nvmf_config:
        destroy_method = get_nvmf_destroy_method(nvmf)
        if destroy_method:
            args.client.call(destroy_method, {'nqn': nvmf['params']['nqn']})


def get_iscsi_destroy_method(iscsi):
    delete_method_map = {'iscsi_create_portal_group': "iscsi_delete_portal_group",
                         'iscsi_create_initiator_group': "iscsi_delete_initiator_group",
                         'iscsi_create_target_node': "iscsi_delete_target_node",
                         'iscsi_set_options': None
                         }
    return delete_method_map[iscsi['method']]


def get_iscsi_name(iscsi):
    if 'name' in iscsi['params']:
        return iscsi['params']['name']
    else:
        return iscsi['params']['tag']


def get_iscsi_name_key(iscsi):
    if iscsi['method'] == 'iscsi_create_target_node':
        return "name"
    else:
        return 'tag'


def clear_iscsi_subsystem(args, iscsi_config):
    for iscsi in iscsi_config:
        destroy_method = get_iscsi_destroy_method(iscsi)
        if destroy_method:
            args.client.call(destroy_method, {get_iscsi_name_key(iscsi): get_iscsi_name(iscsi)})


def get_nbd_destroy_method(nbd):
    delete_method_map = {'nbd_start_disk': "nbd_stop_disk"
                         }
    return delete_method_map[nbd['method']]


def clear_nbd_subsystem(args, nbd_config):
    for nbd in nbd_config:
        destroy_method = get_nbd_destroy_method(nbd)
        if destroy_method:
            args.client.call(destroy_method, {'nbd_device': nbd['params']['nbd_device']})


def clear_net_framework_subsystem(args, net_framework_config):
    pass


def clear_accel_subsystem(args, accel_config):
    pass


def clear_interface_subsystem(args, interface_config):
    pass


def clear_vhost_subsystem(args, vhost_config):
    for vhost in reversed(vhost_config):
        if 'method' in vhost:
            method = vhost['method']
            if method in ['vhost_scsi_controller_add_target']:
                args.client.call("vhost_scsi_controller_remove_target",
                                 {"ctrlr": vhost['params']['ctrlr'],
                                  "scsi_target_num": vhost['params']['scsi_target_num']})
            elif method in ['vhost_create_scsi_controller', 'vhost_create_blk_controller',
                            'vhost_create_nvme_controller']:
                args.client.call("vhost_delete_controller", {'ctrlr': vhost['params']['ctrlr']})


def clear_vmd_subsystem(args, vmd_config):
    pass


def clear_sock_subsystem(args, sock_config):
    pass


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
    parser.add_argument('-v', dest='verbose', action='store_const', const="INFO",
                        help='Set verbose mode to INFO', default="ERROR")
    parser.add_argument('--verbose', dest='verbose', choices=['DEBUG', 'INFO', 'ERROR'],
                        help="""Set verbose level. """)
    subparsers = parser.add_subparsers(help='RPC methods')

    @call_test_cmd
    def clear_config(args):
        for subsystem_item in reversed(args.client.call('framework_get_subsystems')):
            args.subsystem = subsystem_item['subsystem']
            clear_subsystem(args)

    p = subparsers.add_parser('clear_config', help="""Clear configuration of all SPDK subsystems and targets using JSON RPC""")
    p.set_defaults(func=clear_config)

    @call_test_cmd
    def clear_subsystem(args):
        config = args.client.call('framework_get_config', {"name": args.subsystem})
        if config is None:
            return
        if args.verbose:
            print("Calling clear_%s_subsystem" % args.subsystem)
        globals()["clear_%s_subsystem" % args.subsystem](args, config)

    p = subparsers.add_parser('clear_subsystem', help="""Clear configuration of SPDK subsystem using JSON RPC""")
    p.add_argument('--subsystem', help="""Subsystem name""")
    p.set_defaults(func=clear_subsystem)

    args = parser.parse_args()

    with rpc.client.JSONRPCClient(args.server_addr, args.port, args.timeout, log_level=getattr(logging, args.verbose.upper())) as client:
        try:
            args.client = client
            args.func(args)
        except JSONRPCException as ex:
            print((ex.message))
            exit(1)
