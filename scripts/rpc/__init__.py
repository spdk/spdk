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


def get_rpc_methods(args):
    print_dict(args.client.call('get_rpc_methods'))


def save_config(args):
    config = {
        'subsystems': []
    }

    for elem in args.client.call('get_subsystems'):
        rpc_params = {
            "name": elem['subsystem']
        }
        cfg = {
            'subsystem': elem['subsystem'],
            'config': args.client.call('get_subsystem_config', rpc_params)
        }
        config['subsystems'].append(cfg)

    with open(args.filename, 'w') as file:
        json.dump(config, file, indent=2)


def load_config(args):
    with open(args.filename, 'r') as file:
        config = json.load(file)

    for subsystem in config['subsystems']:
        name = subsystem['subsystem']
        config = subsystem['config']
        if not config:
            continue
        for elem in subsystem['config']:
            args.client.call(elem['method'], elem['params'])
