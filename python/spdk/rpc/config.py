#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2017 Intel Corporation.
#  Copyright (C) 2025 Dell Inc, or its subsidiaries.
#  All rights reserved.

from . import client as rpc_client
from .cmd_parser import json_dump as _json_dump
from .cmd_parser import json_load as _json_load


def save_config(client, fd, indent=2, subsystems=None):
    """Write current (live) configuration of SPDK subsystems and targets to stdout.
    Args:
        fd: opened file descriptor where data will be saved
        indent: Indent level. Value less than 0 mean compact mode.
            Default indent level is 2.
        subsystems: subsystems (and their dependencies) to save
    """
    config = {
        'subsystems': []
    }

    subsystems_json = client.framework_get_subsystems()

    # Build a dictionary of all subsystems to their fully expanded set of
    # dependencies.
    dependencies = dict()
    for elem in subsystems_json:
        subsystem = elem['subsystem']
        dependencies[subsystem] = {subsystem}
        for d in elem['depends_on']:
            dependencies[subsystem].update(dependencies[d])

    # Build a set of all of the subsystems to print based on the
    # subsystems parameter.
    to_print = set()
    if subsystems is None:
        to_print = dependencies.keys()
    else:
        for s in subsystems.split(','):
            to_print.update(dependencies[s])

    def _print(x): return x['subsystem'] in to_print
    for elem in filter(_print, subsystems_json):
        cfg = {
            'subsystem': elem['subsystem'],
            'config': client.framework_get_config(name=elem['subsystem'])
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
    allowed_methods = client.rpc_get_methods(include_aliases=include_aliases)
    if not subsystems and 'framework_start_init' in allowed_methods:
        client.framework_start_init()
        return

    for subsystem in list(subsystems):
        config = subsystem['config']
        for elem in list(config):
            if 'method' not in elem or elem['method'] not in allowed_methods:
                raise rpc_client.JSONRPCException("Unknown method was included in the config file")

    while subsystems:
        allowed_methods = client.rpc_get_methods(current=True, include_aliases=include_aliases)
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
            client.framework_start_init()
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
        'config': client.framework_get_config(name=name)
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

    allowed_methods = client.rpc_get_methods()
    config = subsystem['config']
    for elem in list(config):
        if 'method' not in elem or elem['method'] not in allowed_methods:
            raise rpc_client.JSONRPCException("Unknown method was included in the config file")

    allowed_methods = client.rpc_get_methods(current=True)
    for elem in list(config):
        if 'method' not in elem or elem['method'] not in allowed_methods:
            continue

        client.call(**elem)
        config.remove(elem)

    if config:
        print("Some configs were skipped because they cannot be called in the current RPC state.")
