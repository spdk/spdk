import json
import sys

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


def start_subsystem_init(client):
    return client.call('start_subsystem_init')


def get_rpc_methods(client, args):
    params = {}

    if args.current:
        params['current'] = args.current

    return client.call('get_rpc_methods', params)


def __serialize_object(obj, args):
    indent = args.indent
    if args.filename is None:
        if indent is None:
            indent = 2
        elif indent < 0:
            indent = None
        json.dump(obj, sys.stdout, indent=indent)
        sys.stdout.write('\n')
    else:
        if indent is None or indent < 0:
            indent = None
        with open(args.filename, 'w') as file:
            json.dump(obj, file, indent=indent)
            file.write('\n')


def __deserialize_object(args):
    if not args.filename or args.filename == '-':
        obj = json.load(sys.stdin)
    else:
        with open(args.filename, 'r') as file:
            obj = json.load(file)
    return obj


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

    __serialize_object(config, args)


def load_config(client, args):
    config = __deserialize_object(args)

    for subsystem in config['subsystems']:
        name = subsystem['subsystem']
        cfg = subsystem['config']
        if not cfg:
            continue
        for elem in subsystem['config']:
            if not elem or 'method' not in elem:
                continue
            client.call(elem['method'], elem['params'])

def save_option(client, args):
    options = {
        'subsystems': []
    }

    for elem in client.call('get_subsystems'):
        option = {
            'subsystem': elem['subsystem'],
            'option': client.call('get_subsystem_option', {"name": elem['subsystem']})
        }
        options['subsystems'].append(option)

    __serialize_object(options, args)


def load_option(client, args):
    options = __deserialize_object(args)

    for subsystem in options['subsystems']:
        name = subsystem['subsystem']
        option = subsystem['option']
        if not option:
            continue
        for elem in subsystem['option']:
            if not elem or 'method' not in elem:
                continue
            client.call(elem['method'], elem['params'])
