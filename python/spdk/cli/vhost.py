#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2016 Intel Corporation
#  All rights reserved.
#  Copyright (c) 2022 Dell Inc, or its subsidiaries.
#  Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
#

import argparse

from spdk.rpc.client import print_array, print_dict, print_json  # noqa  # noqa
from spdk.rpc.cmd_parser import strip_globals
from spdk.rpc.helpers import DeprecateFalseAction, DeprecateTrueAction


def add_parser(subparsers):

    def vhost_controller_set_coalescing(args):
        args.client.vhost_controller_set_coalescing(
                                                  ctrlr=args.ctrlr,
                                                  delay_base_us=args.delay_base_us,
                                                  iops_threshold=args.iops_threshold)

    p = subparsers.add_parser('vhost_controller_set_coalescing', help='Set vhost controller coalescing')
    p.add_argument('ctrlr', help='controller name')
    p.add_argument('delay_base_us', help='Base delay time', type=int)
    p.add_argument('iops_threshold', help='IOPS threshold when coalescing is enabled', type=int)
    p.set_defaults(func=vhost_controller_set_coalescing)

    def virtio_blk_create_transport(args):
        params = strip_globals(vars(args))
        args.client.virtio_blk_create_transport(**params)

    p = subparsers.add_parser('virtio_blk_create_transport',
                              help='Create virtio blk transport')
    p.add_argument('name', help='transport name')
    p.set_defaults(func=virtio_blk_create_transport)

    def virtio_blk_get_transports(args):
        print_dict(args.client.virtio_blk_get_transports(name=args.name))

    p = subparsers.add_parser('virtio_blk_get_transports', help='Display virtio-blk transports or requested transport')
    p.add_argument('--name', help='Transport name (optional)', type=str)
    p.set_defaults(func=virtio_blk_get_transports)

    def vhost_create_scsi_controller(args):
        args.client.vhost_create_scsi_controller(
                                               ctrlr=args.ctrlr,
                                               cpumask=args.cpumask,
                                               delay=args.delay)

    p = subparsers.add_parser('vhost_create_scsi_controller', help='Add new vhost controller')
    p.add_argument('ctrlr', help='controller name')
    p.add_argument('--cpumask', help='cpu mask for this controller')
    p.add_argument("--delay", action='store_true', help='whether delay starting controller or not')
    p.set_defaults(func=vhost_create_scsi_controller)

    def vhost_start_scsi_controller(args):
        args.client.vhost_start_scsi_controller(ctrlr=args.ctrlr)

    p = subparsers.add_parser('vhost_start_scsi_controller', help='Start vhost scsi controller')
    p.add_argument('ctrlr', help='controller name')
    p.set_defaults(func=vhost_start_scsi_controller)

    def vhost_scsi_controller_add_target(args):
        print_json(args.client.vhost_scsi_controller_add_target(
                                                              ctrlr=args.ctrlr,
                                                              scsi_target_num=args.scsi_target_num,
                                                              bdev_name=args.bdev_name))

    p = subparsers.add_parser('vhost_scsi_controller_add_target', help='Add lun to vhost controller')
    p.add_argument('ctrlr', help='controller name where add lun')
    p.add_argument('scsi_target_num', help='scsi_target_num', type=int)
    p.add_argument('bdev_name', help='bdev name')
    p.set_defaults(func=vhost_scsi_controller_add_target)

    def vhost_scsi_controller_remove_target(args):
        args.client.vhost_scsi_controller_remove_target(
                                                      ctrlr=args.ctrlr,
                                                      scsi_target_num=args.scsi_target_num)

    p = subparsers.add_parser('vhost_scsi_controller_remove_target',
                              help='Remove target from vhost controller')
    p.add_argument('ctrlr', help='controller name to remove target from')
    p.add_argument('scsi_target_num', help='scsi_target_num', type=int)
    p.set_defaults(func=vhost_scsi_controller_remove_target)

    def vhost_create_blk_controller(args):
        params = strip_globals(vars(args))
        args.client.vhost_create_blk_controller(**params)

    p = subparsers.add_parser('vhost_create_blk_controller', help='Add a new vhost block controller')
    p.add_argument('ctrlr', help='controller name')
    p.add_argument('dev_name', help='device name')
    p.add_argument('--cpumask', help='cpu mask for this controller')
    p.add_argument('--transport', help='virtio blk transport name (default: vhost_user_blk)')
    p.add_argument("-r", "--readonly", action='store_true', help='Set controller as read-only')
    p.add_argument("-p", "--packed-ring", action='store_true', help='Set controller as packed ring supported')
    p.set_defaults(func=vhost_create_blk_controller)

    def vhost_get_controllers(args):
        print_dict(args.client.vhost_get_controllers(name=args.name))

    p = subparsers.add_parser('vhost_get_controllers', help='List all or specific vhost controller(s)')
    p.add_argument('-n', '--name', help="Name of vhost controller")
    p.set_defaults(func=vhost_get_controllers)

    def vhost_delete_controller(args):
        args.client.vhost_delete_controller(ctrlr=args.ctrlr)

    p = subparsers.add_parser('vhost_delete_controller', help='Delete a vhost controller')
    p.add_argument('ctrlr', help='controller name')
    p.set_defaults(func=vhost_delete_controller)

    def bdev_virtio_attach_controller(args):
        print_array(args.client.bdev_virtio_attach_controller(
                                                            name=args.name,
                                                            trtype=args.trtype,
                                                            traddr=args.traddr,
                                                            dev_type=args.dev_type,
                                                            vq_count=args.vq_count,
                                                            vq_size=args.vq_size))

    p = subparsers.add_parser('bdev_virtio_attach_controller',
                              help="""Attach virtio controller using provided
    transport type and device type. This will also create bdevs for any block devices connected to the
    controller (for example, SCSI devices for a virtio-scsi controller).
    Result is array of added bdevs.""")
    p.add_argument('name', help="Use this name as base for new created bdevs")
    p.add_argument('-t', '--trtype',
                   help='Virtio target transport type: pci or user', required=True)
    p.add_argument('-a', '--traddr',
                   help='Transport type specific target address: e.g. UNIX domain socket path or BDF', required=True)
    p.add_argument('-d', '--dev-type',
                   help='Device type: blk or scsi', required=True)
    p.add_argument('--vq-count', help='Number of virtual queues to be used.', type=int)
    p.add_argument('--vq-size', help='Size of each queue', type=int)
    p.set_defaults(func=bdev_virtio_attach_controller)

    def bdev_virtio_scsi_get_devices(args):
        print_dict(args.client.bdev_virtio_scsi_get_devices())

    p = subparsers.add_parser('bdev_virtio_scsi_get_devices', help='List all Virtio-SCSI devices.')
    p.set_defaults(func=bdev_virtio_scsi_get_devices)

    def bdev_virtio_detach_controller(args):
        args.client.bdev_virtio_detach_controller(name=args.name)

    p = subparsers.add_parser('bdev_virtio_detach_controller', help="""Remove a Virtio device
    This will delete all bdevs exposed by this device""")
    p.add_argument('name', help='Virtio device name. E.g. VirtioUser0')
    p.set_defaults(func=bdev_virtio_detach_controller)

    def bdev_virtio_blk_set_hotplug(args):
        args.client.bdev_virtio_blk_set_hotplug(enable=args.enable, period_us=args.period_us)

    p = subparsers.add_parser('bdev_virtio_blk_set_hotplug', help='Set hotplug options for bdev virtio_blk type.')
    # TODO: this group is deprecated, remove in next version
    group = p.add_mutually_exclusive_group(required=True)
    group.add_argument('-d', '--disable', dest='enable', action=DeprecateFalseAction, help="Disable hotplug")
    group.add_argument('-e', '--enable', dest='enable', action=DeprecateTrueAction, help="Enable hotplug")
    group.add_argument('--hotplug', dest='enable', action=argparse.BooleanOptionalAction, help='Enable or disable hotplug')
    p.add_argument('-r', '--period-us',
                   help='How often the hotplug is processed for insert and remove events', type=int)
    p.set_defaults(func=bdev_virtio_blk_set_hotplug)
