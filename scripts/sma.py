#!/usr/bin/env python3

from argparse import ArgumentParser
import importlib
import logging
import os
import sys

sys.path.append(os.path.dirname(__file__) + '/../python')

import spdk.sma as sma                      # noqa
from spdk.rpc.client import JSONRPCClient   # noqa


def parse_argv():
    parser = ArgumentParser(description='Storage Management Agent command line interface')
    parser.add_argument('--address', '-a', default='localhost',
                        help='IP address to listen on')
    parser.add_argument('--socket', '-s', default='/var/tmp/spdk.sock',
                        help='SPDK RPC socket')
    parser.add_argument('--port', '-p', default=8080, type=int,
                        help='IP port to listen on')
    return parser.parse_args()


def get_build_client(sock):
    def build_client():
        return JSONRPCClient(sock)

    return build_client


def register_device(agent, device):
    device.init(None)
    agent.register_device(device)


def load_plugins(agent, client, plugins):
    for plugin in plugins:
        module = importlib.import_module(plugin)
        for device in getattr(module, 'devices', []):
            logging.debug(f'Loading external device: {plugin}.{device.__name__}')
            register_device(agent, device(client))


if __name__ == '__main__':
    argv = parse_argv()
    logging.basicConfig(level=os.environ.get('SMA_LOGLEVEL', 'WARNING').upper())
    agent = sma.StorageManagementAgent(argv.address, argv.port)
    register_device(agent, sma.NvmfTcpDeviceManager(get_build_client(argv.socket)))
    load_plugins(agent, get_build_client(argv.socket),
                 filter(None, os.environ.get('SMA_PLUGINS', '').split(':')))
    agent.run()
