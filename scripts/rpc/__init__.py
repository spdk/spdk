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
            if not elem or 'method' not in elem:
                continue
            client.call(elem['method'], elem['params'])
