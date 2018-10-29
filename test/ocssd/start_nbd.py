#!/usr/bin/env python
# BSD LICENSE
#
# Copyright (c) Intel Corporation.
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
#
#   * Redistributions of source code must retain the above copyright
#     notice, this list of conditions and the following disclaimer.
#   * Redistributions in binary form must reproduce the above copyright
#     notice, this list of conditions and the following disclaimer in
#     the documentation and/or other materials provided with the
#     distribution.
#   * Neither the name of Intel Corporation nor the names of its
#     contributors may be used to endorse or promote products derived
#     from this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#

import sys
import os
import datetime

script_path = os.path.join(os.path.dirname(__file__), "../../scripts")
sys.path.append(script_path + '/rpc')

import argparse                         # noqa
import client                           # noqa
from client import JSONRPCException     # noqa


def get_bdevs(client):
    return client.call('get_bdevs')


def start_nbd(client, bdev, nbd):
    params = {'bdev_name': bdev,
              'nbd_device': nbd}
    return client.call('start_nbd_disk', params)


def rpc_connect(client, args, connection_timeout=30):
    # Try connecting for connection_timeout seconds before failing
    end = datetime.datetime.now() + datetime.timedelta(0, connection_timeout)
    while datetime.datetime.now() < end:
        try:
            return client.JSONRPCClient(args.server_addr, args.port, args.verbose, args.timeout)
        except JSONRPCException as ex:
            pass

    return client.JSONRPCClient(args.server_addr, args.port, args.verbose, args.timeout)


def main(args):
    parser = argparse.ArgumentParser(
        description='RPC command line interface')
    parser.add_argument('-s', dest='server_addr',
                        help='RPC server address', default='/var/tmp/spdk.sock')
    parser.add_argument('-p', dest='port',
                        help='RPC port number (if server_addr is IP address)',
                        default=5260, type=int)
    parser.add_argument('-t', dest='timeout',
                        help='Timeout as a floating point number expressed in seconds waiting for '
                             'reponse. Default: 60.0',
                        default=60.0, type=float)
    parser.add_argument('-v', dest='verbose',
                        help='Verbose mode', action='store_true')
    args = parser.parse_args()
    root_dir = os.path.abspath(os.path.dirname(__file__))
    try:
        args.client = rpc_connect(client, args)
        bdevs = get_bdevs(args.client)
        for index, bdev in enumerate(bdevs):
            filename = '.testfile_{0}.{1}'.format(bdev.get("name"), index)
            fbdev = open(os.path.join(root_dir, filename), 'w+')
            fbdev.write(bdev.get("uuid"))
            fbdev.close()
            start_nbd(args.client, bdev.get("name"), "/dev/nbd{0}".format(index))
    except JSONRPCException as ex:
        print(ex.message)
        exit(1)


if __name__ == '__main__':
    main(sys.argv)
