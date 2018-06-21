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


def __json_dump(config, filename, indent):
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


def __json_load(filename):
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

    __json_dump(config, args.filename, args.indent)


def __load_config(client, subsystems, allowed_methods):
    for subsystem in list(subsystems):
        if not subsystem['config']:
            subsystems.remove(subsystem)
            continue

        config = subsystem['config']
        for elem in list(config):
            if 'method' not in elem or elem['method'] not in allowed_methods:
                continue

            client.call(elem['method'], elem['params'])
            config.remove(elem)

        if not config:
            subsystems.remove(subsystem)

    return subsystems


def load_config(client, args):
    json_config = __json_load(args.filename)

    subsystems = json_config['subsystems']
    allowed_methods = client.call('get_rpc_methods', {'current': True})

    if 'start_subsystem_init' not in allowed_methods:
        print("Subsystems have been initialized and some configs are skipped to load")
        __load_config(client, subsystems, allowed_methods)
        return

    subsystems = __load_config(client, subsystems, allowed_methods)
    client.call('start_subsystem_init')

    allowed_methods = client.call('get_rpc_methods', {'current': True})
    subsystems = __load_config(client, subsystems, allowed_methods)

    if subsystems:
        raise rpc_client.JSONRPCException("Loading configs was done but some configs were left")


def load_subsystem_config(client, args):
    config = __json_load(args.filename)

    for elem in config['config']:
        if not elem or 'method' not in elem:
            continue
        client.call(elem['method'], elem['params'])
