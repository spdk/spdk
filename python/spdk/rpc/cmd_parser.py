#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2021 Intel Corporation.
#  All rights reserved.

import io
import json
import os
import sys

try:
    from shlex import quote
except ImportError:
    from pipes import quote


args_global = ['server_addr', 'port', 'timeout', 'verbose', 'dry_run', 'conn_retries',
               'is_server', 'rpc_plugin', 'called_rpc_name', 'func', 'client', 'go_client']


def strip_globals(kwargs):
    return {k: v for k, v in kwargs.items() if k not in args_global}


def remove_null(kwargs):
    return {k: v for k, v in kwargs.items() if v is not None}


def apply_defaults(kwargs, **defaults):
    return {**defaults, **kwargs}


def group_as(kwargs, name, values):
    group = {k: v for k, v in kwargs.items() if k in values and v is not None}
    rest = {k: v for k, v in kwargs.items() if k not in values}
    return {**rest, name: group}


def print_null(arg):
    pass


def print_array(a):
    print(" ".join((quote(v) for v in a)))


def print_dict(d):
    print(json.dumps(d, indent=2))


def print_json(s):
    print(json.dumps(s, indent=2).strip('"'))


def json_dump(config, fd, indent):
    if indent is None:
        indent = 2
    elif indent < 0:
        indent = None
    json.dump(config, fd, indent=indent)
    fd.write('\n')


def json_load(j):
    if j == sys.stdin or isinstance(j, io.IOBase):
        return json.load(j)
    if os.path.exists(j):
        with open(j, "r") as j:
            return json.load(j)
    return json.loads(j)
