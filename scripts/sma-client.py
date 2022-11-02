#!/usr/bin/env python3
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2022 Intel Corporation
#  All rights reserved.
#

from argparse import ArgumentParser
import grpc
import google.protobuf.json_format as json_format
import importlib
import json
import logging
import os
import sys

sys.path.append(os.path.dirname(__file__) + '/../python')

import spdk.sma.proto.sma_pb2 as sma_pb2                        # noqa
import spdk.sma.proto.sma_pb2_grpc as sma_pb2_grpc              # noqa
import spdk.sma.proto.nvmf_tcp_pb2 as nvmf_tcp_pb2              # noqa
import spdk.sma.proto.nvmf_tcp_pb2_grpc as nvmf_tcp_pb2_grpc    # noqa


class Client:
    def __init__(self, addr, port):
        self._service = sma_pb2.DESCRIPTOR.services_by_name['StorageManagementAgent']
        self.addr = addr
        self.port = port

    def _get_message_type(self, descriptor):
        return getattr(sma_pb2, descriptor.name)

    def _get_method_types(self, method_name):
        method = self._service.methods_by_name.get(method_name)
        return (self._get_message_type(method.input_type),
                self._get_message_type(method.output_type))

    def call(self, method, params):
        with grpc.insecure_channel(f'{self.addr}:{self.port}') as channel:
            stub = sma_pb2_grpc.StorageManagementAgentStub(channel)
            func = getattr(stub, method)
            input, output = self._get_method_types(method)
            response = func(request=json_format.ParseDict(params, input()))
            return json_format.MessageToDict(response,
                                             preserving_proto_field_name=True)


def load_plugins(plugins):
    for plugin in plugins:
        logging.debug(f'Loading external plugin: {plugin}')
        module = importlib.import_module(plugin)


def parse_argv():
    parser = ArgumentParser(description='Storage Management Agent client')
    parser.add_argument('--address', '-a', default='localhost',
                        help='IP address of SMA instance to connect to')
    parser.add_argument('--port', '-p', default=8080, type=int,
                        help='Port number of SMA instance to connect to')
    return parser.parse_args()


def main(args):
    argv = parse_argv()
    logging.basicConfig(level=os.environ.get('SMA_LOGLEVEL', 'WARNING').upper())
    load_plugins(filter(None, os.environ.get('SMA_PLUGINS', '').split(':')))
    client = Client(argv.address, argv.port)
    request = json.loads(sys.stdin.read())
    result = client.call(request['method'], request.get('params', {}))
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main(sys.argv[1:])
