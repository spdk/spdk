#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2016 Intel Corporation
#  All rights reserved.
#  Copyright (c) 2022 Dell Inc, or its subsidiaries.
#  Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
#

from spdk.rpc.cmd_parser import print_dict, print_json


def add_parser(subparsers):

    def bdev_lvol_create_lvstore(args):
        # The default unmap clear method may take over 60.0 sec.
        if args.timeout is None:
            args.client.timeout = 90.0
        print_json(args.client.bdev_lvol_create_lvstore(
                                                     bdev_name=args.bdev_name,
                                                     lvs_name=args.lvs_name,
                                                     cluster_sz=args.cluster_sz,
                                                     clear_method=args.clear_method,
                                                     num_md_pages_per_cluster_ratio=args.num_md_pages_per_cluster_ratio,
                                                     md_page_size=args.md_page_size))

    p = subparsers.add_parser('bdev_lvol_create_lvstore', help='Add logical volume store on base bdev')
    p.add_argument('bdev_name', help='Bdev on which to construct the logical volume store')
    p.add_argument('lvs_name', help='Name of the logical volume store to create')
    p.add_argument('-c', '--cluster-sz', help='Cluster size of the logical volume store in bytes',
                   type=int)
    p.add_argument('--clear-method', choices=['none', 'unmap', 'write_zeroes'],
                   help='Clear method for the data region')
    p.add_argument('-m', '--md-pages-per-cluster-ratio', dest='num_md_pages_per_cluster_ratio',
                   help='Reserved metadata pages per cluster, as a percentage', type=int)
    p.add_argument('-s', '--md-page-size',
                   help='Metadata page size in bytes', type=int)
    p.set_defaults(func=bdev_lvol_create_lvstore)

    def bdev_lvol_rename_lvstore(args):
        args.client.bdev_lvol_rename_lvstore(
                                          old_name=args.old_name,
                                          new_name=args.new_name)

    p = subparsers.add_parser('bdev_lvol_rename_lvstore', help='Change logical volume store name')
    p.add_argument('old_name', help='Existing logical volume store name')
    p.add_argument('new_name', help='New logical volume store name')
    p.set_defaults(func=bdev_lvol_rename_lvstore)

    def bdev_lvol_grow_lvstore(args):
        print_dict(args.client.bdev_lvol_grow_lvstore(
                                                   uuid=args.uuid,
                                                   lvs_name=args.lvs_name))

    p = subparsers.add_parser('bdev_lvol_grow_lvstore',
                              help='Grow the lvstore size to the underlying bdev size')
    p.add_argument('-u', '--uuid', help='UUID of the logical volume store to grow')
    p.add_argument('-l', '--lvs-name', help='Name of the logical volume store to grow')
    p.set_defaults(func=bdev_lvol_grow_lvstore)

    def bdev_lvol_create(args):
        print_json(args.client.bdev_lvol_create(
                                             lvol_name=args.lvol_name,
                                             size_in_mib=args.size_in_mib,
                                             thin_provision=args.thin_provision,
                                             clear_method=args.clear_method,
                                             uuid=args.uuid,
                                             lvs_name=args.lvs_name))

    p = subparsers.add_parser('bdev_lvol_create', help='Add a bdev with an logical volume backend')
    p.add_argument('-u', '--uuid', help='UUID of the logical volume store')
    p.add_argument('-l', '--lvs-name', help='Name of the logical volume store')
    p.add_argument('-t', '--thin-provision', action='store_true', help='Enable thin provisioning')
    p.add_argument('-c', '--clear-method', choices=['default', 'none', 'unmap', 'write_zeroes'],
                   help='Per-cluster clear method')
    p.add_argument('lvol_name', help='Name of the logical volume to create')
    p.add_argument('size_in_mib', help='Desired size of the logical volume in MiB', type=int)
    p.set_defaults(func=bdev_lvol_create)

    def bdev_lvol_snapshot(args):
        print_json(args.client.bdev_lvol_snapshot(
                                               lvol_name=args.lvol_name,
                                               snapshot_name=args.snapshot_name))

    p = subparsers.add_parser('bdev_lvol_snapshot', help='Create a snapshot of an lvol bdev')
    p.add_argument('lvol_name', help='UUID or alias of the logical volume to create a snapshot from')
    p.add_argument('snapshot_name', help='Name for the newly created snapshot')
    p.set_defaults(func=bdev_lvol_snapshot)

    def bdev_lvol_clone(args):
        print_json(args.client.bdev_lvol_clone(
                                            snapshot_name=args.snapshot_name,
                                            clone_name=args.clone_name))

    p = subparsers.add_parser('bdev_lvol_clone', help='Create a clone of an lvol snapshot')
    p.add_argument('snapshot_name', help='UUID or alias of the snapshot to clone')
    p.add_argument('clone_name', help='Name for the logical volume to create')
    p.set_defaults(func=bdev_lvol_clone)

    def bdev_lvol_clone_bdev(args):
        print_json(args.client.bdev_lvol_clone_bdev(
                                                 bdev=args.bdev,
                                                 lvs_name=args.lvs_name,
                                                 clone_name=args.clone_name))

    p = subparsers.add_parser('bdev_lvol_clone_bdev',
                              help='Create a clone of a non-lvol bdev')
    p.add_argument('bdev', help='Name or UUID for bdev that acts as the external snapshot')
    p.add_argument('lvs_name', help='Name of the logical volume store')
    p.add_argument('clone_name', help='Name for the logical volume to create')
    p.set_defaults(func=bdev_lvol_clone_bdev)

    def bdev_lvol_rename(args):
        args.client.bdev_lvol_rename(
                                  old_name=args.old_name,
                                  new_name=args.new_name)

    p = subparsers.add_parser('bdev_lvol_rename', help='Change lvol bdev name')
    p.add_argument('old_name', help='UUID or alias of the existing logical volume')
    p.add_argument('new_name', help='New logical volume name')
    p.set_defaults(func=bdev_lvol_rename)

    def bdev_lvol_inflate(args):
        args.client.bdev_lvol_inflate(name=args.name)

    p = subparsers.add_parser('bdev_lvol_inflate', help='Make thin provisioned lvol a thick provisioned lvol')
    p.add_argument('name', help='UUID or alias of the logical volume to inflate')
    p.set_defaults(func=bdev_lvol_inflate)

    def bdev_lvol_decouple_parent(args):
        args.client.bdev_lvol_decouple_parent(name=args.name)

    p = subparsers.add_parser('bdev_lvol_decouple_parent', help='Decouple parent of lvol')
    p.add_argument('name', help='UUID or alias of the logical volume to decouple from its parent')
    p.set_defaults(func=bdev_lvol_decouple_parent)

    def bdev_lvol_resize(args):
        args.client.bdev_lvol_resize(
                                  name=args.name,
                                  size_in_mib=args.size_in_mib)

    p = subparsers.add_parser('bdev_lvol_resize', help='Resize existing lvol bdev')
    p.add_argument('name', help='UUID or alias of the logical volume to resize')
    p.add_argument('size_in_mib', help='Desired size of the logical volume in MiB', type=int)
    p.set_defaults(func=bdev_lvol_resize)

    def bdev_lvol_set_read_only(args):
        args.client.bdev_lvol_set_read_only(name=args.name)

    p = subparsers.add_parser('bdev_lvol_set_read_only', help='Mark lvol bdev as read only')
    p.add_argument('name', help='UUID or alias of the logical volume to set as read only')
    p.set_defaults(func=bdev_lvol_set_read_only)

    def bdev_lvol_delete(args):
        args.client.bdev_lvol_delete(name=args.name)

    p = subparsers.add_parser('bdev_lvol_delete', help='Destroy a logical volume')
    p.add_argument('name', help='UUID or alias of the logical volume to destroy')
    p.set_defaults(func=bdev_lvol_delete)

    def bdev_lvol_start_shallow_copy(args):
        print_json(args.client.bdev_lvol_start_shallow_copy(
                                                         src_lvol_name=args.src_lvol_name,
                                                         dst_bdev_name=args.dst_bdev_name))

    p = subparsers.add_parser('bdev_lvol_start_shallow_copy',
                              help="""Start a shallow copy of an lvol over a given bdev.  The status of the operation
    can be obtained with bdev_lvol_check_shallow_copy""")
    p.add_argument('src_lvol_name', help='UUID or alias of lvol to create a copy from')
    p.add_argument('dst_bdev_name', help='Name of the bdev that acts as destination for the copy')
    p.set_defaults(func=bdev_lvol_start_shallow_copy)

    def bdev_lvol_check_shallow_copy(args):
        print_json(args.client.bdev_lvol_check_shallow_copy(operation_id=args.operation_id))

    p = subparsers.add_parser('bdev_lvol_check_shallow_copy', help='Get shallow copy status')
    p.add_argument('operation_id', help='operation identifier', type=int)
    p.set_defaults(func=bdev_lvol_check_shallow_copy)

    def bdev_lvol_set_parent(args):
        args.client.bdev_lvol_set_parent(
                                      lvol_name=args.lvol_name,
                                      parent_name=args.parent_name)

    p = subparsers.add_parser('bdev_lvol_set_parent', help='Set the parent snapshot of a lvol')
    p.add_argument('lvol_name', help='UUID or alias of the lvol to set parent of')
    p.add_argument('parent_name', help='UUID or alias of the snapshot to become parent of lvol')
    p.set_defaults(func=bdev_lvol_set_parent)

    def bdev_lvol_set_parent_bdev(args):
        args.client.bdev_lvol_set_parent_bdev(
                                           lvol_name=args.lvol_name,
                                           parent_name=args.parent_name)

    p = subparsers.add_parser('bdev_lvol_set_parent_bdev', help='Set the parent external snapshot of a lvol')
    p.add_argument('lvol_name', help='UUID or alias of the lvol to set external parent of')
    p.add_argument('parent_name', help='UUID or name of the external snapshot to become parent of lvol')
    p.set_defaults(func=bdev_lvol_set_parent_bdev)

    def bdev_lvol_delete_lvstore(args):
        args.client.bdev_lvol_delete_lvstore(
                                          uuid=args.uuid,
                                          lvs_name=args.lvs_name)

    p = subparsers.add_parser('bdev_lvol_delete_lvstore', help='Destroy an logical volume store')
    p.add_argument('-u', '--uuid', help='UUID of the logical volume store to destroy')
    p.add_argument('-l', '--lvs-name', help='Name of the logical volume store to destroy')
    p.set_defaults(func=bdev_lvol_delete_lvstore)

    def bdev_lvol_get_lvstores(args):
        print_dict(args.client.bdev_lvol_get_lvstores(
                                                   uuid=args.uuid,
                                                   lvs_name=args.lvs_name))

    p = subparsers.add_parser('bdev_lvol_get_lvstores', help='Display current logical volume store list')
    p.add_argument('-u', '--uuid', help='UUID of the logical volume store to retrieve information about')
    p.add_argument('-l', '--lvs-name', help='Name of the logical volume store to retrieve information about')
    p.set_defaults(func=bdev_lvol_get_lvstores)

    def bdev_lvol_get_lvols(args):
        print_dict(args.client.bdev_lvol_get_lvols(
                                                lvs_uuid=args.lvs_uuid,
                                                lvs_name=args.lvs_name))

    p = subparsers.add_parser('bdev_lvol_get_lvols', help='Display current logical volume list')
    p.add_argument('-u', '--lvs-uuid', help='Only show volumes in the logical volume store with this UUID')
    p.add_argument('-l', '--lvs-name', help='Only show volumes in the logical volume store with this name')
    p.set_defaults(func=bdev_lvol_get_lvols)
