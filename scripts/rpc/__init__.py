import json
import os
import sys

from io import IOBase as io

from . import app
from . import bdev
from . import blobfs
from . import env_dpdk
from . import idxd
from . import ioat
from . import iscsi
from . import log
from . import lvol
from . import nbd
from . import notify
from . import nvme
from . import nvmf
from . import pmem
from . import subsystem
from . import trace
from . import vhost
from . import vmd
from . import sock
from . import client as rpc_client
from .helpers import deprecated_alias


@deprecated_alias('start_subsystem_init')
def framework_start_init(client):
    """Start initialization of subsystems"""
    return client.call('framework_start_init')


@deprecated_alias('wait_subsystem_init')
def framework_wait_init(client):
    """Block until subsystems have been initialized"""
    return client.call('framework_wait_init')


@deprecated_alias("get_rpc_methods")
def rpc_get_methods(client, current=None, include_aliases=None):
    """Get list of supported RPC methods.
    Args:
        current: Get list of RPC methods only callable in the current state.
        include_aliases: Include aliases in the list with RPC methods.
    """
    params = {}

    if current:
        params['current'] = current
    if include_aliases:
        params['include_aliases'] = include_aliases

    return client.call('rpc_get_methods', params)


@deprecated_alias("get_spdk_version")
def spdk_get_version(client):
    """Get SPDK version"""
    return client.call('spdk_get_version')


def _json_dump(config, fd, indent):
    if indent is None:
        indent = 2
    elif indent < 0:
        indent = None
    json.dump(config, fd, indent=indent)
    fd.write('\n')


def _json_load(j):
    if j == sys.stdin or isinstance(j, io):
        json_conf = json.load(j)
    elif os.path.exists(j):
        with open(j, "r") as j:
            json_conf = json.load(j)
    else:
        json_conf = json.loads(j)
    return json_conf


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

    for elem in client.call('framework_get_subsystems'):
        cfg = {
            'subsystem': elem['subsystem'],
            'config': client.call('framework_get_config', {"name": elem['subsystem']})
        }
        config['subsystems'].append(cfg)

    _json_dump(config, fd, indent)


def load_config(client, fd, include_aliases=False):
    """Configure SPDK subsystems and targets using JSON RPC read from stdin.
    Args:
        fd: opened file descriptor where data will be taken from
    """
    json_config = _json_load(fd)

    # remove subsystems with no config
    subsystems = json_config['subsystems']
    for subsystem in list(subsystems):
        if not subsystem['config']:
            subsystems.remove(subsystem)

    # check if methods in the config file are known
    allowed_methods = client.call('rpc_get_methods', {'include_aliases': include_aliases})
    if not subsystems and 'framework_start_init' in allowed_methods:
        framework_start_init(client)
        return

    for subsystem in list(subsystems):
        config = subsystem['config']
        for elem in list(config):
            if 'method' not in elem or elem['method'] not in allowed_methods:
                raise rpc_client.JSONRPCException("Unknown method was included in the config file")

    while subsystems:
        allowed_methods = client.call('rpc_get_methods', {'current': True,
                                                          'include_aliases': include_aliases})
        allowed_found = False

        for subsystem in list(subsystems):
            config = subsystem['config']
            for elem in list(config):
                if 'method' not in elem or elem['method'] not in allowed_methods:
                    continue

                client.call(**elem)
                config.remove(elem)
                allowed_found = True

            if not config:
                subsystems.remove(subsystem)

        if 'framework_start_init' in allowed_methods:
            framework_start_init(client)
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
        'config': client.call('framework_get_config', {"name": name})
    }

    _json_dump(cfg, fd, indent)


def load_subsystem_config(client, fd):
    """Configure SPDK subsystem using JSON RPC read from stdin.
    Args:
        fd: opened file descriptor where data will be taken from
    """
    subsystem = _json_load(fd)

    if not subsystem['config']:
        return

    allowed_methods = client.call('rpc_get_methods')
    config = subsystem['config']
    for elem in list(config):
        if 'method' not in elem or elem['method'] not in allowed_methods:
            raise rpc_client.JSONRPCException("Unknown method was included in the config file")

    allowed_methods = client.call('rpc_get_methods', {'current': True})
    for elem in list(config):
        if 'method' not in elem or elem['method'] not in allowed_methods:
            continue

        client.call(**elem)
        config.remove(elem)

    if config:
        print("Some configs were skipped because they cannot be called in the current RPC state.")
