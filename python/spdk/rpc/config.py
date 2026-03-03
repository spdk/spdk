#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2017 Intel Corporation.
#  Copyright (C) 2025 Dell Inc, or its subsidiaries.
#  All rights reserved.

from . import client as rpc_client
from .cmd_parser import json_dump as _json_dump
from .cmd_parser import json_load as _json_load


def save_config(client, fd, indent=2, subsystems=None, with_batches=False):
    """Write current (live) configuration of SPDK subsystems and targets to stdout.
    Args:
        fd: opened file descriptor where data will be saved
        indent: Indent level. Value less than 0 mean compact mode.
            Default indent level is 2.
        subsystems: subsystems (and their dependencies) to save
        with_batches: If True, include batch arrays in output. If False, flatten them.
    """
    config = {
        'subsystems': [],
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
            'config': client.framework_get_config(name=elem['subsystem'], with_batches=with_batches),
        }
        config['subsystems'].append(cfg)

    _json_dump(config, fd, indent)


def _check_allowed_method(elem, allowed_methods, raise_on_fail=False):
    """Check if elem's method is in allowed_methods.
    Returns True if allowed, False otherwise.
    If raise_on_fail is True, raises JSONRPCException instead of returning False.
    """
    if 'method' not in elem or elem['method'] not in allowed_methods:
        if raise_on_fail:
            raise rpc_client.JSONRPCException("Unknown method was included in the config file")
        return False
    return True


def _validate_config_elem(elem, allowed_methods):
    """Validate a config element (single RPC or batch array).
    Raises JSONRPCException if invalid.
    """
    if isinstance(elem, list):
        for item in elem:
            _check_allowed_method(item, allowed_methods, raise_on_fail=True)
        return

    _check_allowed_method(elem, allowed_methods, raise_on_fail=True)


def _process_config_elem(client, elem, config, allowed_methods):
    """Process a config element (single RPC or batch array).
    Returns True if any method was called, False otherwise.
    Removes processed items from elem/config.
    """
    if isinstance(elem, list):
        for item in elem:
            if not _check_allowed_method(item, allowed_methods):
                return False

        if not elem:
            config.remove(elem)
            return False

        client.call_batch(elem)
        config.remove(elem)
        return True

    if not _check_allowed_method(elem, allowed_methods):
        return False
    client.call(**elem)
    config.remove(elem)
    return True


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
            _validate_config_elem(elem, allowed_methods)

    while subsystems:
        allowed_methods = client.rpc_get_methods(current=True, include_aliases=include_aliases)
        allowed_found = False

        for subsystem in list(subsystems):
            config = subsystem['config']
            for elem in list(config):
                if _process_config_elem(client, elem, config, allowed_methods):
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


def save_subsystem_config(client, fd, indent=2, name=None, with_batches=False):
    """Write current (live) configuration of SPDK subsystem to stdout.
    Args:
        fd: opened file descriptor where data will be saved
        indent: Indent level. Value less than 0 mean compact mode.
            Default is indent level 2.
        with_batches: If True, include batch arrays in output. If False, flatten them.
    """
    cfg = {
        'subsystem': name,
        'config': client.framework_get_config(name=name, with_batches=with_batches),
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
        _validate_config_elem(elem, allowed_methods)

    allowed_methods = client.rpc_get_methods(current=True)
    for elem in list(config):
        _process_config_elem(client, elem, config, allowed_methods)

    if config:
        print("Some configs were skipped because they cannot be called in the current RPC state.")
