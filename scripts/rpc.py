#!/usr/bin/env python3
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2016 Intel Corporation
#  All rights reserved.
#  Copyright (c) 2022 Dell Inc, or its subsidiaries.
#  Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
#

import logging
import argparse
import importlib
import os
import sys
import shlex
import json
import types

sys.path.insert(0, os.path.dirname(__file__) + '/../python')

import spdk.cli as cli  # noqa
from spdk.rpc.client import JSONRPCClient, JSONRPCGoClient, JSONRPCException  # noqa
from spdk.rpc.helpers import deprecated_aliases, hint_rpc_name  # noqa
from spdk.rpc.cmd_parser import remove_null  # noqa


def create_parser():
    parser = argparse.ArgumentParser(
        description='SPDK RPC command line interface', usage='%(prog)s [options]')
    parser.add_argument('-s', dest='server_addr',
                        help='RPC domain socket path or IP address', default='/var/tmp/spdk.sock')
    parser.add_argument('-p', dest='port',
                        help='RPC port number (if server_addr is IP address)',
                        default=5260, type=int)
    parser.add_argument('-t', dest='timeout',
                        help='Timeout as a floating point number expressed in seconds waiting for response. Default: 60.0',
                        default=None, type=float)
    parser.add_argument('-r', dest='conn_retries',
                        help='Retry connecting to the RPC server N times with 0.2s interval. Default: 0',
                        default=0, type=int)
    parser.add_argument('-v', dest='verbose', action='store_const', const="INFO",
                        help='Set verbose mode to INFO', default="ERROR")
    parser.add_argument('--verbose', dest='verbose', choices=['DEBUG', 'INFO', 'ERROR'],
                        help="""Set verbose level. """)
    parser.add_argument('--dry-run', dest='dry_run', action='store_true', help="Display request and exit")
    parser.set_defaults(dry_run=False)
    parser.add_argument('--go-client', dest='go_client', action='store_true', help="Use Go client")
    parser.set_defaults(go_client=False)
    parser.add_argument('--server', dest='is_server', action='store_true',
                        help="Start listening on stdin, parse each line as a regular rpc.py execution and create \
                                a separate connection for each command. Each command's output ends with either \
                                **STATUS=0 if the command succeeded or **STATUS=1 if it failed. --server is meant \
                                to be used in conjunction with bash coproc, where stdin and stdout are connected to \
                                pipes and can be used as a faster way to send RPC commands. If enabled, rpc.py \
                                must be executed without any other parameters.")
    parser.set_defaults(is_server=False)
    parser.add_argument('--plugin', dest='rpc_plugin', help='Module name of plugin with additional RPC commands')
    subparsers = parser.add_subparsers(help='RPC methods', dest='called_rpc_name', metavar='')
    for name, obj in vars(cli).items():
        if isinstance(obj, types.ModuleType) and obj.__name__.startswith("spdk.cli."):
            obj.add_parser(subparsers)
    return parser, subparsers


def check_called_name(name):
    if name in deprecated_aliases:
        print("{} is deprecated, use {} instead.".format(name, deprecated_aliases[name]), file=sys.stderr)


class dry_run_client:
    def __getattr__(self, name):
        return lambda **kwargs: self.call(name, remove_null(kwargs))

    def call(self, method, params=None):
        print("Request:\n" + json.dumps({"method": method, "params": params}, indent=2))


def null_print(arg):
    pass


def call_rpc_func(args):
    args.func(args)
    check_called_name(args.called_rpc_name)


def execute_script(parser, client, timeout, fd):
    executed_rpc = ""
    for rpc_call in map(str.rstrip, fd):
        if not rpc_call.strip():
            continue
        executed_rpc = "\n".join([executed_rpc, rpc_call])
        rpc_args = shlex.split(rpc_call)
        if rpc_args[0][0] == '#':
            # Ignore lines starting with # - treat them as comments
            continue
        args = parser.parse_args(rpc_args)
        args.client = client
        args.timeout = timeout
        try:
            call_rpc_func(args)
        except JSONRPCException as ex:
            print("Exception:")
            print(executed_rpc.strip() + " <<<")
            print(ex.message)
            exit(1)


def load_plugin(args, subparsers, plugins):
    # Create temporary parser, pull out the plugin parameter, load the module, and then run the real argument parser
    plugin_parser = argparse.ArgumentParser(add_help=False)
    plugin_parser.add_argument('--plugin', dest='rpc_plugin', help='Module name of plugin with additional RPC commands')

    rpc_module = plugin_parser.parse_known_args()[0].rpc_plugin
    if args is not None:
        rpc_module = plugin_parser.parse_known_args(args)[0].rpc_plugin

    if rpc_module in plugins:
        return

    if rpc_module is not None:
        try:
            rpc_plugin = importlib.import_module(rpc_module)
            try:
                rpc_plugin.spdk_rpc_plugin_initialize(subparsers)
                plugins.append(rpc_module)
            except AttributeError:
                print("Module %s does not contain 'spdk_rpc_plugin_initialize' function" % rpc_module)
        except ModuleNotFoundError:
            print("Module %s not found" % rpc_module)


def replace_arg_underscores(args):
    # All option names are defined with dashes only - for example: --tgt-name
    # But if user used underscores, convert them to dashes (--tgt_name => --tgt-name)
    # SPDK was inconsistent previously and had some options with underscores, so
    # doing this conversion ensures backward compatibility with older scripts.
    for i in range(len(args)):
        arg = args[i]
        if arg.startswith('--') and "_" in arg:
            opt, *vals = arg.split('=')
            args[i] = '='.join([opt.replace('_', '-'), *vals])


def main():

    parser, subparsers = create_parser()

    plugins = []
    load_plugin(None, subparsers, plugins)

    replace_arg_underscores(sys.argv)

    parser = hint_rpc_name(parser)
    args = parser.parse_args()

    try:
        use_go_client = int(os.getenv('SPDK_JSONRPC_GO_CLIENT', 0)) == 1
    except ValueError:
        use_go_client = False

    if sys.stdin.isatty() and not hasattr(args, 'func'):
        # No arguments and no data piped through stdin
        parser.print_help()
        exit(1)
    if args.is_server:
        for input in sys.stdin:
            cmd = shlex.split(input)
            replace_arg_underscores(cmd)
            try:
                load_plugin(cmd, subparsers, plugins)
                tmp_args = parser.parse_args(cmd)
            except SystemExit as ex:
                print("**STATUS=1", flush=True)
                continue

            try:
                if use_go_client:
                    tmp_args.client = JSONRPCGoClient(tmp_args.server_addr,
                                                      log_level=getattr(logging, tmp_args.verbose.upper()))
                else:
                    tmp_args.client = JSONRPCClient(
                        tmp_args.server_addr, tmp_args.port, tmp_args.timeout,
                        log_level=getattr(logging, tmp_args.verbose.upper()), conn_retries=tmp_args.conn_retries)
                call_rpc_func(tmp_args)
                print("**STATUS=0", flush=True)
            except JSONRPCException as ex:
                print(ex.message)
                print("**STATUS=1", flush=True)
        exit(0)
    elif args.dry_run:
        args.client = dry_run_client()
        for name, obj in vars(cli).items():
            if isinstance(obj, types.ModuleType) and obj.__name__.startswith("spdk.cli."):
                obj.print_dict = obj.print_json = obj.print_array = null_print
    elif args.go_client or use_go_client:
        try:
            args.client = JSONRPCGoClient(args.server_addr,
                                          log_level=getattr(logging, args.verbose.upper()))
        except JSONRPCException as ex:
            print(ex.message)
            exit(1)
    else:
        try:
            args.client = JSONRPCClient(args.server_addr, args.port, args.timeout,
                                        log_level=getattr(logging, args.verbose.upper()),
                                        conn_retries=args.conn_retries)
        except JSONRPCException as ex:
            print(ex.message)
            exit(1)

    if hasattr(args, 'func'):
        try:
            call_rpc_func(args)
        except JSONRPCException as ex:
            print(ex.message)
            exit(1)
    else:
        execute_script(parser, args.client, args.timeout, sys.stdin)


if __name__ == "__main__":
    main()
