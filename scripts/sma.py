#!/usr/bin/env python3
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2022 Intel Corporation
#  All rights reserved.
#

from argparse import ArgumentParser
import importlib
import logging
import os
import signal
import sys
import threading
import time
import yaml

sys.path.append(os.path.dirname(__file__) + '/../python')

import spdk.sma as sma               # noqa
import spdk.rpc.client as rpcclient  # noqa


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
                'port': 8080,
                'discovery_timeout': 10.0,
                'volume_cleanup_period': 60.0}
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
        return rpcclient.JSONRPCClient(sock)

    return build_client


def register_devices(agent, devices, config):
    for device_config in config.get('devices') or []:
        name = device_config.get('name')
        device_manager = next(filter(lambda s: s.name == name, devices), None)
        if device_manager is None:
            logging.error(f'Couldn\'t find device: {name}')
            sys.exit(1)
        logging.info(f'Registering device: {name}')
        device_manager.init(device_config.get('params'))
        agent.register_device(device_manager)


def init_crypto(config, client):
    crypto_config = config.get('crypto')
    if crypto_config is None:
        return
    name = crypto_config.get('name')
    if name is None:
        logging.error('Crypto engine name is missing')
        sys.exit(1)
    try:
        sma.set_crypto_engine(name)
        sma.get_crypto_engine().init(client, crypto_config.get('params', {}))
    except ValueError:
        logging.error(f'Invalid crypto engine: {name}')
        sys.exit(1)


def load_plugins(plugins, client):
    devices = []
    for plugin in plugins:
        module = importlib.import_module(plugin)
        for device in getattr(module, 'devices', []):
            logging.debug(f'Loading external device: {plugin}.{device.__name__}')
            devices.append(device(client))
        for engine_class in getattr(module, 'crypto_engines', []):
            engine = engine_class()
            logging.debug(f'Loading external crypto engine: {plugin}.{engine.name}')
            sma.register_crypto_engine(engine)
    return devices


def wait_for_listen(client, timeout):
    start = time.monotonic()
    while True:
        try:
            with client() as _client:
                _client.call('rpc_get_methods')
            # If we got here, the process is responding to RPCs
            break
        except rpcclient.JSONRPCException:
            logging.debug('The SPDK process is not responding for {}s'.format(
                          int(time.monotonic() - start)))

        if time.monotonic() > start + timeout:
            logging.error('Timed out while waiting for SPDK process to respond')
            sys.exit(1)
        time.sleep(1)


def run(agent):
    event = threading.Event()

    def signal_handler(signum, frame):
        event.set()

    for signum in [signal.SIGTERM, signal.SIGINT]:
        signal.signal(signum, signal_handler)

    agent.start()
    event.wait()
    agent.stop()


if __name__ == '__main__':
    logging.basicConfig(level=os.environ.get('SMA_LOGLEVEL', 'WARNING').upper())

    config = parse_argv()
    client = get_build_client(config['socket'])

    # Wait until the SPDK process starts responding to RPCs
    wait_for_listen(client, timeout=60.0)

    agent = sma.StorageManagementAgent(config, client)

    devices = [sma.NvmfTcpDeviceManager(client), sma.VhostBlkDeviceManager(client),
               sma.NvmfVfioDeviceManager(client)]
    devices += load_plugins(config.get('plugins') or [], client)
    devices += load_plugins(filter(None, os.environ.get('SMA_PLUGINS', '').split(':')),
                            client)
    init_crypto(config, client)
    register_devices(agent, devices, config)
    run(agent)
