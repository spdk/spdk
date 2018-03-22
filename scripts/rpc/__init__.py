import app
import bdev
from client import print_dict
import iscsi
import log
import lvol
import nbd
import net
import nvmf
import pmem
import subsystem
import vhost
import json
import sys
import subprocess


def get_rpc_methods(args):
    print_dict(args.client.call('get_rpc_methods'))


def save_config(args):
    config = {
        'subsystems': []
    }

    for elem in args.client.call('get_subsystems'):
        cfg = {
            'subsystem': elem['subsystem'],
            'config': args.client.call('get_subsystem_config', {"name": elem['subsystem']})
        }
        config['subsystems'].append(cfg)

    indent = args.indent
    if args.filename is None:
        if indent is None:
            indent = 2
        elif indent < 0:
            indent = None
        json.dump(config, sys.stdout, indent=indent)
        sys.stdout.write('\n')
    else:
        if indent is None or indent < 0:
            indent = None
        with open(args.filename, 'w') as file:
            json.dump(config, file, indent=indent)
            file.write('\n')


def load_config(args):
    if not args.filename or args.filename == '-':
        config = json.load(sys.stdin)
    else:
        with open(args.filename, 'r') as file:
            config = json.load(file)

    for subsystem in config['subsystems']:
        name = subsystem['subsystem']
        config = subsystem['config']
        if not config:
            continue
        for elem in subsystem['config']:
            args.client.call(elem['method'], elem['params'])


def clear_config(args):
    for subsystem in args.client.call('get_subsystems'):
        print("subsystem: %s" % subsystem)
        args.subsystem = subsystem['subsystem']
        clear_subsystem(args)


def clear_subsystem(args):
    config = args.client.call('get_subsystem_config', {"name": args.subsystem})
    if config is None:
        return
    globals()["clear_%s_subsystem" % args.subsystem](args, config)


def clear_bdev_subsystem(args, bdev_config):
    for bdev in reversed(bdev_config):
        print("bdev: %s" % bdev)
        if 'name' in bdev:
            args.client.call("delete_bdev", {'name': bdev['name']})
        elif 'params' in bdev and 'name' in bdev['params']:
            args.client.call("delete_bdev", {'name': bdev['params']['name']})
        elif 'method' in bdev and bdev['method'] == 'construct_split_vbdev':
            args.client.call("destruct_split_vbdev", {'base_bdev': bdev['params']['base_bdev']})
        elif 'method' in bdev and bdev['method'] == 'construct_pmem_bdev':
            args.client.call("delete_bdev", {'name': bdev['params']['name']})


def clear_nbd_subsystem(args, nbd_config):
   for nbd in nbd_config:
        if 'name' in nbd:
            args.client.call("stop_nbd_disk", {'name': nbd['name']})


def clear_vhost_subsystem(args, vhost_config):
    pass


def clear_scsi_subsystem(args, scsi_config):
    pass


def clear_copy_subsystem(args, copy_config):
    pass


def clear_interface_subsystem(args, interface_config):
    pass


def clear_net_framework_subsystem(args, net_framework_config):
    pass
