import json
import sys
import subprocess

from . import app
from . import bdev
from . import iscsi
from . import log
from . import lvol
from . import nbd
from . import net
from . import nvmf
from . import pmem
from . import subsystem
from . import vhost


def get_rpc_methods(client):
    return client.call('get_rpc_methods')


def save_config(client, args):
    config = {
        'subsystems': []
    }

    for elem in client.call('get_subsystems'):
        cfg = {
            'subsystem': elem['subsystem'],
            'config': client.call('get_subsystem_config', {"name": elem['subsystem']})
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


def load_config(client, args):
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
            client.call(elem['method'], elem['params'])


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
    for vhost in reversed(vhost_config):
        print("vhost: %s" % vhost)
        if 'method' in vhost and bdev['method'] in 'construct_vhost_scsi_controller':
            args.client.call("remove_vhost_controller", {'name': bdev['params']['ctrl']})
        elif 'method' in bdev and bdev['method'] in 'construct_vhost_blk_controller':
            args.client.call("remove_vhost_controller", {'name': bdev['params']['ctrl']})


def clear_scsi_subsystem(args, scsi_config):
    pass


def clear_copy_subsystem(args, copy_config):
    pass


def clear_interface_subsystem(args, interface_config):
    pass


def clear_net_framework_subsystem(args, net_framework_config):
    pass
