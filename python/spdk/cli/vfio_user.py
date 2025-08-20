#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2016 Intel Corporation
#  All rights reserved.
#  Copyright (c) 2022 Dell Inc, or its subsidiaries.
#  Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
#

import sys
from spdk.rpc.client import print_dict, print_json, print_array  # noqa


def add_parser(subparsers):

    def vfu_tgt_set_base_path(args):
        args.client.vfu_tgt_set_base_path(path=args.path)

    p = subparsers.add_parser('vfu_tgt_set_base_path', help='Set socket base path.')
    p.add_argument('path', help='socket base path')
    p.set_defaults(func=vfu_tgt_set_base_path)

    def vfu_virtio_delete_endpoint(args):
        args.client.vfu_virtio_delete_endpoint(name=args.name)

    p = subparsers.add_parser('vfu_virtio_delete_endpoint', help='Delete the PCI device via endpoint name.')
    p.add_argument('name', help='Endpoint name')
    p.set_defaults(func=vfu_virtio_delete_endpoint)

    def vfu_virtio_create_blk_endpoint(args):
        args.client.vfu_virtio_create_blk_endpoint(
                                                     name=args.name,
                                                     bdev_name=args.bdev_name,
                                                     cpumask=args.cpumask,
                                                     num_queues=args.num_queues,
                                                     qsize=args.qsize,
                                                     packed_ring=args.packed_ring)

    p = subparsers.add_parser('vfu_virtio_create_blk_endpoint', help='Create virtio-blk endpoint.')
    p.add_argument('name', help='Name of the endpoint')
    p.add_argument('--bdev-name', help='block device name', type=str, required=True)
    p.add_argument('--cpumask', help='CPU masks')
    p.add_argument('--num-queues', help='number of vrings', type=int, default=0)
    p.add_argument('--qsize', help='number of element for each vring', type=int, default=0)
    p.add_argument("--packed-ring", action='store_true', help='Enable packed ring')
    p.set_defaults(func=vfu_virtio_create_blk_endpoint)

    def vfu_virtio_scsi_add_target(args):
        args.client.vfu_virtio_scsi_add_target(
                                                 name=args.name,
                                                 scsi_target_num=args.scsi_target_num,
                                                 bdev_name=args.bdev_name)

    p = subparsers.add_parser('vfu_virtio_scsi_add_target', help='Attach a block device to SCSI target of PCI endpoint.')
    p.add_argument('name', help='Name of the endpoint')
    p.add_argument('--scsi-target-num', help='number of SCSI Target', type=int, required=True)
    p.add_argument('--bdev-name', help='block device name', type=str, required=True)
    p.set_defaults(func=vfu_virtio_scsi_add_target)

    def vfu_virtio_scsi_remove_target(args):
        args.client.vfu_virtio_scsi_remove_target(
                                                    name=args.name,
                                                    scsi_target_num=args.scsi_target_num)

    p = subparsers.add_parser('vfu_virtio_scsi_remove_target', help='Remove the specified SCSI target of PCI endpoint.')
    p.add_argument('name', help='Name of the endpoint')
    p.add_argument('--scsi-target-num', help='number of SCSI Target', type=int, required=True)
    p.set_defaults(func=vfu_virtio_scsi_remove_target)

    def vfu_virtio_create_scsi_endpoint(args):
        args.client.vfu_virtio_create_scsi_endpoint(
                                                      name=args.name,
                                                      cpumask=args.cpumask,
                                                      num_io_queues=args.num_io_queues,
                                                      qsize=args.qsize,
                                                      packed_ring=args.packed_ring)

    p = subparsers.add_parser('vfu_virtio_create_scsi_endpoint', help='Create virtio-scsi endpoint.')
    p.add_argument('name', help='Name of the endpoint')
    p.add_argument('--cpumask', help='CPU masks')
    p.add_argument('--num-io-queues', help='number of IO vrings', type=int, default=0)
    p.add_argument('--qsize', help='number of element for each vring', type=int, default=0)
    p.add_argument("--packed-ring", action='store_true', help='Enable packed ring')
    p.set_defaults(func=vfu_virtio_create_scsi_endpoint)

    def vfu_virtio_create_fs_endpoint(args):
        args.client.vfu_virtio_create_fs_endpoint(
                                                    name=args.name,
                                                    fsdev_name=args.fsdev_name,
                                                    tag=args.tag,
                                                    cpumask=args.cpumask,
                                                    num_queues=args.num_queues,
                                                    qsize=args.qsize,
                                                    packed_ring=args.packed_ring)

    p = subparsers.add_parser('vfu_virtio_create_fs_endpoint', help='Create virtio-fs endpoint.')
    p.add_argument('name', help='Name of the endpoint')
    p.add_argument('--fsdev-name', help='fsdev name', type=str, required=True)
    p.add_argument('--tag', help='virtiofs tag', type=str, required=True)
    p.add_argument('--cpumask', help='CPU masks')
    p.add_argument('--num-queues', help='number of vrings', type=int, default=0)
    p.add_argument('--qsize', help='number of element for each vring', type=int, default=0)
    p.add_argument("--packed-ring", action='store_true', help='Enable packed ring')
    p.set_defaults(func=vfu_virtio_create_fs_endpoint)
