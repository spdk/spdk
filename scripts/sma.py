#!/usr/bin/env python3

from argparse import ArgumentParser
import importlib
import logging
import os
import sys
import yaml

sys.path.append(os.path.dirname(__file__) + '/../python')

import spdk.sma as sma                      # noqa
from spdk.rpc.client import JSONRPCClient   # noqa


def parse_config(path):
    if path is None:
        return {}
    with open(path, 'r') as cfgfile:
        config = yaml.load(cfgfile, Loader=yaml.FullLoader)
        return {**config} if config is not None else {}


def parse_argv():
    parser = ArgumentParser(description='Storage Management Agent command line interface')
    parser.add_argument('--address', '-a', help='IP address to listen on')
    parser.add_argument('--socket', '-s', help='SPDK RPC socket')
    parser.add_argument('--port', '-p', type=int, help='IP port to listen on')
    parser.add_argument('--config', '-c', help='Path to config file')
    defaults = {'address': 'localhost',
                'socket': '/var/tmp/spdk.sock',
                'port': 8080}
    # Merge the default values, config file, and the command-line
    args = vars(parser.parse_args())
    config = parse_config(args.get('config'))
    for argname, argvalue in defaults.items():
        if args.get(argname) is not None:
            if config.get(argname) is not None:
                logging.info(f'Overriding "{argname}" value from command-line')
            config[argname] = args[argname]
        if config.get(argname) is None:
            config[argname] = argvalue
    return config


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
    logging.basicConfig(level=os.environ.get('SMA_LOGLEVEL', 'WARNING').upper())

    config = parse_argv()
    agent = sma.StorageManagementAgent(config['address'], config['port'])
    register_device(agent, sma.NvmfTcpDeviceManager(get_build_client(config['socket'])))
    load_plugins(agent, get_build_client(config['socket']), config.get('plugins') or [])
    load_plugins(agent, get_build_client(config['socket']),
                 filter(None, os.environ.get('SMA_PLUGINS', '').split(':')))
    agent.run()
