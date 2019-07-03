#!/usr/bin/env python3

import logging
import argparse
import sys
import shlex

try:
    from rpc.client import print_dict, JSONRPCException
    import rpc
except ImportError:
    print("SPDK RPC library missing. Please add spdk/scripts/ directory to PYTHONPATH:")
    print("'export PYTHONPATH=$PYTHONPATH:./spdk/scripts/'")
    exit(1)

try:
    from shlex import quote
except ImportError:
    from pipes import quote


def print_array(a):
    print(" ".join((quote(v) for v in a)))


def perform_tests_func(client):
    """Perform bdevperf tests according to command line arguments when application was started.

    Args:
        none

    Returns:
        On success, 0 is returned. On error, -1 is returned.
    """
    params = {}
    return client.call('perform_tests', params)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description='SPDK RPC command line interface. NOTE: spdk/scripts/ is expected in PYTHONPATH')
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
        print_dict(perform_tests_func(args.client))

    p = subparsers.add_parser('perform_tests', help='Perform bdevperf tests')
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
    args.client = rpc.client.JSONRPCClient(args.server_addr, args.port, args.timeout, log_level=getattr(logging, args.verbose.upper()))
    if hasattr(args, 'func'):
        call_rpc_func(args)
    elif sys.stdin.isatty():
        # No arguments and no data piped through stdin
        parser.print_help()
        exit(1)
    else:
        execute_script(parser, args.client, sys.stdin)
