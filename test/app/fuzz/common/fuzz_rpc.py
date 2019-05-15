#!/usr/bin/env python3

from rpc.client import print_dict, JSONRPCException

import logging
import argparse
import rpc
import sys
import shlex

try:
    from shlex import quote
except ImportError:
    from pipes import quote


def print_array(a):
    print(" ".join((quote(v) for v in a)))


def _fuzz_vhost_create_dev(client, socket, is_blk, use_bogus_buffer, use_valid_buffer, test_scsi_tmf, valid_lun):
    """Create a new device in the vhost fuzzer.

    Args:
        socket: A valid unix domain socket for the dev to bind to.
        is_blk: if set, create a virtio_blk device, otherwise use scsi.
        use_bogus_buffer: if set, pass an invalid memory address as a buffer accompanying requests.
        use_valid_buffer: if set, pass in a valid memory buffer with requests. Overrides use_bogus_buffer.
        test_scsi_tmf: Test scsi management commands on the given device. Valid if and only if is_blk is false.
        valid_lun: Supply only a valid lun number when submitting commands to the given device. Valid if and only if is_blk is false.

    Returns:
        True or False
    """

    params = {"socket": socket,
              "is_blk": is_blk,
              "use_bogus_buffer": use_bogus_buffer,
              "use_valid_buffer": use_valid_buffer,
              "test_scsi_tmf": test_scsi_tmf,
              "valid_lun": valid_lun}

    return client.call("fuzz_vhost_create_dev", params)


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

    def fuzz_vhost_create_dev(args):
        _fuzz_vhost_create_dev(
            args.client,
            args.socket,
            args.is_blk,
            args.use_bogus_buffer,
            args.use_valid_buffer,
            args.test_scsi_tmf,
            args.valid_lun)

    p = subparsers.add_parser('fuzz_vhost_create_dev', help="Add a new device to the vhost fuzzer.")
    p.add_argument('-s', '--socket', help="Path to a valid unix domain socket for dev binding.")
    p.add_argument('-b', '--is-blk', help='The specified socket corresponds to a vhost-blk dev.', action='store_true')
    p.add_argument('-u', '--use-bogus-buffer', help='Pass bogus buffer addresses with requests when fuzzing.', action='store_true')
    p.add_argument('-v', '--use-valid-buffer', help='Pass valid buffers when fuzzing. overrides use-bogus-buffer.', action='store_true')
    p.add_argument('-m', '--test-scsi-tmf', help='for a scsi device, test scsi management commands.', action='store_true')
    p.add_argument('-l', '--valid-lun', help='for a scsi device, test only using valid lun IDs.', action='store_true')
    p.set_defaults(func=fuzz_vhost_create_dev)

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
