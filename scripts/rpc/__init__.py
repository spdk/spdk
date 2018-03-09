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

    if not args.filename or args.filename == '-':
        indent = args.indent if hasattr(args, 'indent') else 4
        json.dump(config, sys.stdout, indent=indent)
        sys.stdout.write('\n')
    else:
        with open(args.filename, 'w') as file:
            indent = args.indent if hasattr(args, 'indent') else None
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
