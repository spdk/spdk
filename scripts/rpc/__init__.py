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


def _json_dump(config, filename, indent):
    if filename is None:
        if indent is None:
            indent = 2
        elif indent < 0:
            indent = None
        json.dump(config, sys.stdout, indent=indent)
        sys.stdout.write('\n')
    else:
        if indent is None or indent < 0:
            indent = None
        with open(filename, 'w') as file:
            json.dump(config, file, indent=indent)
            file.write('\n')


def _json_load(filename):
    if not filename or filename == '-':
        return json.load(sys.stdin)

    else:
        with open(filename, 'r') as file:
            return json.load(file)


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

    _json_dump(config, args.filename, args.indent)


def load_config(client, args):
    json_config = _json_load(args.filename)

    # remove subsystems with no config
    subsystems = json_config['subsystems']
    for subsystem in list(subsystems):
        if not subsystem['config']:
            subsystems.remove(subsystem)

    # check if methods in the config file are known
    allowed_methods = client.call('get_rpc_methods')
    for subsystem in list(subsystems):
        config = subsystem['config']
        for elem in list(config):
            if 'method' not in elem or elem['method'] not in allowed_methods:
                raise rpc_client.JSONRPCException("Unknown method was included in the config file")

    while subsystems:
        allowed_methods = client.call('get_rpc_methods', {'current': True})
        allowed_found = False

        for subsystem in list(subsystems):
            config = subsystem['config']
            for elem in list(config):
                if 'method' not in elem or elem['method'] not in allowed_methods:
                    continue

                client.call(elem['method'], elem['params'])
                config.remove(elem)
                allowed_found = True

            if not config:
                subsystems.remove(subsystem)

        if 'start_subsystem_init' in allowed_methods:
            client.call('start_subsystem_init')
            allowed_found = True

        if not allowed_found:
            break

    if subsystems:
        print("Some configs were skipped because the RPC state that can call them passed over.")


def save_subsystem_config(client, args):
    cfg = {
        'subsystem': args.name,
        'config': client.call('get_subsystem_config', {"name": args.name})
    }

    _json_dump(cfg, args.filename, args.indent)


def load_subsystem_config(client, args):
    subsystem = _json_load(args.filename)

    if not subsystem['config']:
        return

    allowed_methods = client.call('get_rpc_methods')
    config = subsystem['config']
    for elem in list(config):
        if 'method' not in elem or elem['method'] not in allowed_methods:
            raise rpc_client.JSONRPCException("Unknown method was included in the config file")

    allowed_methods = client.call('get_rpc_methods', {'current': True})
    for elem in list(config):
        if 'method' not in elem or elem['method'] not in allowed_methods:
            continue

        client.call(elem['method'], elem['params'])
        config.remove(elem)

    if config:
        print("Some configs were skipped because they cannot be called in the current RPC state.")
