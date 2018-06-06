import json
import sys

from . import app
from . import bdev
from . import ioat
from . import iscsi
from . import log
from . import lvol
from . import nbd
from . import net
from . import nvmf
from . import pmem
from . import subsystem
from . import vhost
from . import client as rpc_client


def start_subsystem_init(client):
    return client.call('start_subsystem_init')


def get_rpc_methods(client, args):
    params = {}

    if args.current:
        params['current'] = args.current

    return client.call('get_rpc_methods', params)


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
        json_config = json.load(sys.stdin)
    else:
        with open(args.filename, 'r') as file:
            json_config = json.load(file)

    subsystems = json_config['subsystems']
    while subsystems:
        allowed_methods = client.call('get_rpc_methods', {'current': True})
        allowed_found = False

        for subsystem in list(subsystems):
            if not subsystem['config']:
                subsystems.remove(subsystem)
                continue

            config = subsystem['config']
            for elem in list(config):
                if not elem or 'method' not in elem or elem['method'] not in allowed_methods:
                    continue

                client.call(elem['method'], elem['params'])
                config.remove(elem)
                allowed_found = True

            if not config:
                subsystems.remove(subsystem)

        if 'start_subsystem_init' in allowed_methods:
            client.call('start_subsystem_init')
            allowed_found = True

        if subsystems and not allowed_found:
            raise rpc_client.JSONRPCException("Some config left but did not found any allowed method to execute")


def load_subsystem_config(client, args):
    if not args.filename or args.filename == '-':
        config = json.load(sys.stdin)
    else:
        with open(args.filename, 'r') as file:
            config = json.load(file)

    for elem in config['config']:
        if not elem or 'method' not in elem:
            continue
        client.call(elem['method'], elem['params'])
