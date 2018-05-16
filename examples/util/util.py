#!/usr/bin/env python

import argparse

import os
import sys

scrips_rpc = os.path.abspath(os.path.join(os.path.dirname(__file__), '../../scripts/'))
sys.path.append(scrips_rpc)
import rpc
from rpc.client import print_dict, JSONRPCException

""" This ia dirty hack. We should rename rpc.py or the module :( """
import imp
with open("/".join((scrips_rpc, "rpc.py"))) as source:
    spdk_rpc = imp.load_module("spdk_rpc", source, "../../scripts/rpc.py", ('', '', imp.PY_SOURCE))

def dd_rpc(client, args):
    params = vars(args)
    for k in params.keys():
        if k not in ('bs', 'count', 'if', 'ibs', 'obs', 'of', 'seek', 'skip'):
            params.pop(k)
        elif not params[k]:
           params.pop(k)
    return print_dict(client.call('dd', params))


@spdk_rpc.call_cmd
def dd(args):
    dd_rpc(args.client, args)

def build_parser(parser):
    subparsers = spdk_rpc.build_parsers(parser)
    p = subparsers.add_parser('dd', help='Disk Distroyer Util')
    p.add_argument('--bs', help='Read and write up to BYTES bytes at a time', type=int)
    p.add_argument('--count', help='Copy only COUNT input blocks', type=int)
    p.add_argument('--if', help='Source (input) bdev name', required=True)
    p.add_argument('--ibs', help='Read up to BYTES bytes at a time', type=int)
    p.add_argument('--obs', help='Write OBS bytes at a time', type=int)
    p.add_argument('--of', help='Destination (output) bdev name', required=True)
    p.add_argument('--seek', help='Skip SEEK obs-sized blocks as start of output', type=int)
    p.add_argument('--skip', help='Skip SEEK ibs-sized blocks as start of input', type=int)
    p.set_defaults(func=dd)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
    description='SPDK RPC command line interface for examples DD')
    build_parser(parser)

    args = parser.parse_args()
    try:
        args.client = rpc.client.JSONRPCClient(args.server_addr, args.port, args.verbose, args.timeout)
    except JSONRPCException as ex:
        print(ex.message)
        exit(1)
    args.func(args)
