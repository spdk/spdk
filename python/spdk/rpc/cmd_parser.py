#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2021 Intel Corporation.
#  All rights reserved.

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
