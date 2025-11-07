#!/usr/bin/env python3
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2019 Intel Corporation
#  All rights reserved.
#

import argparse
import logging
import os
import shlex
import sys

try:
    from shlex import quote
except ImportError:
    from pipes import quote

try:
    sys.path.append(os.path.dirname(__file__) + '/../../../python')
    from spdk.rpc.client import print_dict, JSONRPCClient, JSONRPCException  # noqa
except ImportError:
    print("SPDK RPC library missing. Please add spdk/python directory to PYTHONPATH:")
    print("'export PYTHONPATH=$PYTHONPATH:spdk/python'")
    exit(1)


PATTERN_TYPES_STR = ("read", "write", "randread", "randwrite", "rw", "randrw", "verify", "reset",
                     "unmap", "flush", "write_zeroes")


def print_array(a):
    print(" ".join((quote(v) for v in a)))


def perform_tests_func(args):
    """Perform bdevperf tests with command line arguments.

    Args:
        none

    Returns:
        On success, 0 is returned. On error, -1 is returned.
    """
    client = args.client
    param_names = ['queue_depth', 'time_in_sec', 'workload_type', 'io_size', 'rw_percentage']
    params = {name: getattr(args, name) for name in param_names if getattr(args, name, None)}
    return client.call('perform_tests', params)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description='SPDK RPC command line interface. NOTE: spdk/python is expected in PYTHONPATH')
    parser.add_argument('-s', dest='server_addr',
                        help='RPC domain socket path or IP address', default='/var/tmp/spdk.sock')
    parser.add_argument('-p', dest='port',
                        help='RPC port number (if server_addr is IP address)',
                        default=5260, type=int)
    parser.add_argument('-t', dest='timeout',
                        help='Timeout as a floating point number expressed in seconds waiting for response. Default: 60.0',
                        default=60.0, type=float)
    parser.add_argument('-v', dest='verbose', action='store_const', const="INFO",
                        help='Set verbose mode to INFO', default="ERROR")
    parser.add_argument('--verbose', dest='verbose', choices=['DEBUG', 'INFO', 'ERROR'],
                        help="""Set verbose level. """)
    subparsers = parser.add_subparsers(help='RPC methods')

    def perform_tests(args):
        print_dict(perform_tests_func(args))

    perform_tests_help_string = '''
    Perform bdevperf tests
    All parameters are optional.
    If any parameter provided it will overwrite ones from prior run or command line.
    If job config file was used no parameter should be provided.
    '''
    p = subparsers.add_parser('perform_tests', help=perform_tests_help_string)
    p.add_argument('-q', dest="queue_depth", help='io depth', type=int)
    p.add_argument('-o', dest="io_size", help='Size in bytes', type=str)
    p.add_argument('-t', dest="time_in_sec", help='Time in seconds', type=int)
    p.add_argument('-M', dest="rw_percentage", help='rwmixread (100 for reads, 0 for writes)',
                   type=int, choices=range(0, 101), metavar="[0-100]")
    p.add_argument('-w', dest="workload_type", choices=PATTERN_TYPES_STR, type=str.lower,
                   help=f'io pattern type, must be one of {PATTERN_TYPES_STR}',)
    p.set_defaults(func=perform_tests)

    def call_rpc_func(args):
        try:
            args.func(args)
        except JSONRPCException as ex:
            print(ex.message)
            exit(1)

    def execute_script(parser, client, fd):
        for rpc_call in map(str.rstrip, fd):
            if not rpc_call.strip():
                continue
            args = parser.parse_args(shlex.split(rpc_call))
            args.client = client
            call_rpc_func(args)

    args = parser.parse_args()
    if args.time_in_sec is not None:
        args.timeout = max(float(args.time_in_sec + 5), args.timeout)
    args.client = JSONRPCClient(args.server_addr, args.port, args.timeout, log_level=getattr(logging, args.verbose.upper()))
    if hasattr(args, 'func'):
        call_rpc_func(args)
    elif sys.stdin.isatty():
        # No arguments and no data piped through stdin
        parser.print_help()
        exit(1)
    else:
        execute_script(parser, args.client, sys.stdin)
