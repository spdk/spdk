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
from . import nvme
from . import nvmf
from . import pmem
from . import subsystem
from . import vhost
from . import client as rpc_client


def start_subsystem_init(client):
    """Start initialization of subsystems"""
    return client.call('start_subsystem_init')


def get_rpc_methods(client, current=None):
    """Get list of supported RPC methods.
    Args:
        current: Get list of RPC methods only callable in the current state.
    """
    params = {}

    if current:
        params['current'] = current

    return client.call('get_rpc_methods', params)


def _json_dump(config, fd, indent):
    if indent is None:
        indent = 2
    elif indent < 0:
        indent = None
    json.dump(config, fd, indent=indent)
    fd.write('\n')


def save_config(client, fd, indent=2):
    """Write current (live) configuration of SPDK subsystems and targets to stdout.
    Args:
        fd: opened file descriptor where data will be saved
        indent: Indent level. Value less than 0 mean compact mode.
            Default indent level is 2.
    """
    config = {
        'subsystems': []
    }

    for elem in client.call('get_subsystems'):
        cfg = {
            'subsystem': elem['subsystem'],
            'config': client.call('get_subsystem_config', {"name": elem['subsystem']})
        }
        config['subsystems'].append(cfg)

    _json_dump(config, fd, indent)


def load_config(client, fd):
    """Configure SPDK subsystems and targets using JSON RPC read from stdin.
    Args:
        fd: opened file descriptor where data will be taken from
    """
    json_config = json.load(fd)

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


def save_subsystem_config(client, fd, indent=2, name=None):
    """Write current (live) configuration of SPDK subsystem to stdout.
    Args:
        fd: opened file descriptor where data will be saved
        indent: Indent level. Value less than 0 mean compact mode.
            Default is indent level 2.
    """
    cfg = {
        'subsystem': name,
        'config': client.call('get_subsystem_config', {"name": name})
    }

    _json_dump(cfg, fd, indent)


def load_subsystem_config(client, fd):
    """Configure SPDK subsystem using JSON RPC read from stdin.
    Args:
        fd: opened file descriptor where data will be taken from
    """
    subsystem = json.load(fd)

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
