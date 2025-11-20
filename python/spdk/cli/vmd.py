#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2016 Intel Corporation
#  All rights reserved.
#  Copyright (c) 2022 Dell Inc, or its subsidiaries.
#  Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
#

from spdk.rpc.cmd_parser import print_dict


def add_parser(subparsers):

    def vmd_enable(args):
        print_dict(args.client.vmd_enable())

    p = subparsers.add_parser('vmd_enable', help='Enable VMD enumeration')
    p.set_defaults(func=vmd_enable)

    def vmd_remove_device(args):
        print_dict(args.client.vmd_remove_device(addr=args.addr))

    p = subparsers.add_parser('vmd_remove_device', help='Remove a device behind VMD')
    p.add_argument('addr', help='Address of the device to remove', type=str)
    p.set_defaults(func=vmd_remove_device)

    def vmd_rescan(args):
        print_dict(args.client.vmd_rescan())

    p = subparsers.add_parser('vmd_rescan', help='Force a rescan of the devices behind VMD')
    p.set_defaults(func=vmd_rescan)
