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
    import os
    print os.system("pwd")
    proc = subprocess.Popen(["python", args.rpc, "-s", args.server_addr, "save_config"], stdout=subprocess.PIPE)
    output = proc.communicate()[0]
    config = json.loads(output)
    for subsystem in config['subsystems']:
        print "subsystem: %s" % subsystem
        if subsystem['subsystem'] == "bdev":
            clear_bdev_subsystem(args, subsystem['config'])


def clear_subsystem(args):
    proc = subprocess.Popen(["python", args.rpc, "-s", args.server_addr, "get_subsystem", args.subsystem], stdout=subprocess.PIPE)
    output = proc.communicate()[0]
    config = json.loads(output)
    if args.subsystem == 'bdev':
        clear_bdev_subsystem(config)

def clear_bdev_subsystem(args, bdev_config):
    for bdev in bdev_config:
        print "bdev: %s" % bdev
        if 'name' in bdev:
            args.client.call("delete_bdev", {'name': bdev['name']})
        #elif 'params' in bdev and 'name' in bdev['params']:
        #    args.client.call("delete_bdev", {'name': bdev['params']['name']})
