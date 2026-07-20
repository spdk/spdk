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

    p = subparsers.add_parser('bdev_lvol_create_lvstore', help="Construct a logical volume store.")
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

    p = subparsers.add_parser('bdev_lvol_rename_lvstore', help="Rename a logical volume store.")
    p.add_argument('old_name', help='Existing logical volume store name')
    p.add_argument('new_name', help='New logical volume store name')
    p.set_defaults(func=bdev_lvol_rename_lvstore)

    def bdev_lvol_grow_lvstore(args):
        print_dict(args.client.bdev_lvol_grow_lvstore(
                                                   uuid=args.uuid,
                                                   lvs_name=args.lvs_name))

    p = subparsers.add_parser('bdev_lvol_grow_lvstore',
                              help="Grow the logical volume store to fill the underlying bdev Growth is"
                                   " limited by the number of metadata pages allocated when the lvstore"
                                   " was created, and its specific max size can be found using the"
                                   " `max_growable_size` parameter from the bdev_lvol_get_lvstores RPC."
                                   " This RPC can be called to grow an lvolstore when the underlying"
                                   " bdev has increased in size.")
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

    p = subparsers.add_parser('bdev_lvol_create', help="Create a logical volume on a logical volume store.")
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

    p = subparsers.add_parser('bdev_lvol_snapshot', help="Capture a snapshot of the current state of a logical volume.")
    p.add_argument('lvol_name', help='UUID or alias of the logical volume to create a snapshot from')
    p.add_argument('snapshot_name', help='Name for the newly created snapshot')
    p.set_defaults(func=bdev_lvol_snapshot)

    def bdev_lvol_clone(args):
        print_json(args.client.bdev_lvol_clone(
                                            snapshot_name=args.snapshot_name,
                                            clone_name=args.clone_name))

    p = subparsers.add_parser('bdev_lvol_clone', help="Create a logical volume based on a snapshot.")
    p.add_argument('snapshot_name', help='UUID or alias of the snapshot to clone')
    p.add_argument('clone_name', help='Name for the logical volume to create')
    p.set_defaults(func=bdev_lvol_clone)

    def bdev_lvol_clone_bdev(args):
        print_json(args.client.bdev_lvol_clone_bdev(
                                                 bdev=args.bdev,
                                                 lvs_name=args.lvs_name,
                                                 clone_name=args.clone_name))

    p = subparsers.add_parser('bdev_lvol_clone_bdev',
                              help="Create a logical volume based on an external snapshot bdev. The"
                                   " external snapshot bdev is a bdev that will not be written to by any"
                                   " consumer and must not be an lvol in the lvstore as the clone."
                                   " Regardless of whether the bdev is specified by name or UUID, the"
                                   " bdev UUID will be stored in the logical volume's metadata for use"
                                   " while the lvolstore is loading. For this reason, it is important"
                                   " that the bdev chosen has a static UUID.")
    p.add_argument('bdev', help='Name or UUID for bdev that acts as the external snapshot')
    p.add_argument('lvs_name', help='Name of the logical volume store')
    p.add_argument('clone_name', help='Name for the logical volume to create')
    p.set_defaults(func=bdev_lvol_clone_bdev)

    def bdev_lvol_rename(args):
        args.client.bdev_lvol_rename(
                                  old_name=args.old_name,
                                  new_name=args.new_name)

    p = subparsers.add_parser('bdev_lvol_rename',
                              help="Rename a logical volume. New name will rename only the alias of the logical volume.")
    p.add_argument('old_name', help='UUID or alias of the existing logical volume')
    p.add_argument('new_name', help='New logical volume name')
    p.set_defaults(func=bdev_lvol_rename)

    def bdev_lvol_inflate(args):
        args.client.bdev_lvol_inflate(name=args.name)

    p = subparsers.add_parser('bdev_lvol_inflate',
                              help="Inflate a logical volume. All unallocated clusters are allocated and"
                                   " copied from the parent or zero filled if not allocated in the"
                                   " parent. Then all dependencies on the parent are removed.")
    p.add_argument('name', help='UUID or alias of the logical volume to inflate')
    p.set_defaults(func=bdev_lvol_inflate)

    def bdev_lvol_decouple_parent(args):
        args.client.bdev_lvol_decouple_parent(name=args.name)

    p = subparsers.add_parser('bdev_lvol_decouple_parent',
                              help="Decouple the parent of a logical volume. For unallocated clusters"
                                   " which is allocated in the parent, they are allocated and copied from"
                                   " the parent, but for unallocated clusters which is thin provisioned"
                                   " in the parent, they are kept thin provisioned. Then all dependencies"
                                   " on the parent are removed.")
    p.add_argument('name', help='UUID or alias of the logical volume to decouple from its parent')
    p.set_defaults(func=bdev_lvol_decouple_parent)

    def bdev_lvol_resize(args):
        args.client.bdev_lvol_resize(
                                  name=args.name,
                                  size_in_mib=args.size_in_mib)

    p = subparsers.add_parser('bdev_lvol_resize', help="Resize a logical volume.")
    p.add_argument('name', help='UUID or alias of the logical volume to resize')
    p.add_argument('size_in_mib', help='Desired size of the logical volume in MiB', type=int)
    p.set_defaults(func=bdev_lvol_resize)

    def bdev_lvol_set_read_only(args):
        args.client.bdev_lvol_set_read_only(name=args.name)

    p = subparsers.add_parser('bdev_lvol_set_read_only', help="Mark logical volume as read only.")
    p.add_argument('name', help='UUID or alias of the logical volume to set as read only')
    p.set_defaults(func=bdev_lvol_set_read_only)

    def bdev_lvol_delete(args):
        args.client.bdev_lvol_delete(name=args.name)

    p = subparsers.add_parser('bdev_lvol_delete', help="Destroy a logical volume.")
    p.add_argument('name', help='UUID or alias of the logical volume to destroy')
    p.set_defaults(func=bdev_lvol_delete)

    def bdev_lvol_start_shallow_copy(args):
        print_json(args.client.bdev_lvol_start_shallow_copy(
                                                         src_lvol_name=args.src_lvol_name,
                                                         dst_bdev_name=args.dst_bdev_name))

    p = subparsers.add_parser('bdev_lvol_start_shallow_copy',
                              help="Start a shallow copy of an lvol over a given bdev. Only clusters"
                                   " allocated to the lvol will be written on the bdev. Must have:"
                                   " * lvol read only * lvol size less or equal than bdev size"
                                   " * lvstore block size an even multiple of bdev block size")
    p.add_argument('src_lvol_name', help='UUID or alias of lvol to create a copy from')
    p.add_argument('dst_bdev_name', help='Name of the bdev that acts as destination for the copy')
    p.set_defaults(func=bdev_lvol_start_shallow_copy)

    def bdev_lvol_check_shallow_copy(args):
        print_json(args.client.bdev_lvol_check_shallow_copy(operation_id=args.operation_id))

    p = subparsers.add_parser('bdev_lvol_check_shallow_copy', help="Get shallow copy status.")
    p.add_argument('operation_id', help='operation identifier', type=int)
    p.set_defaults(func=bdev_lvol_check_shallow_copy)

    def bdev_lvol_set_parent(args):
        args.client.bdev_lvol_set_parent(
                                      lvol_name=args.lvol_name,
                                      parent_name=args.parent_name)

    p = subparsers.add_parser('bdev_lvol_set_parent',
                              help="Set a snapshot as the parent of a lvol, making the lvol a clone of"
                                   " this snapshot. The previous parent of the lvol, if any, can be"
                                   " another snapshot or an external snapshot; if the lvol is not a"
                                   " clone, it must be thin-provisioned. Lvol and parent snapshot must"
                                   " have the same size and must belong to the same lvol store.")
    p.add_argument('lvol_name', help='UUID or alias of the lvol to set parent of')
    p.add_argument('parent_name', help='UUID or alias of the snapshot to become parent of lvol')
    p.set_defaults(func=bdev_lvol_set_parent)

    def bdev_lvol_set_parent_bdev(args):
        args.client.bdev_lvol_set_parent_bdev(
                                           lvol_name=args.lvol_name,
                                           parent_name=args.parent_name)

    p = subparsers.add_parser('bdev_lvol_set_parent_bdev',
                              help="Set an external snapshot as the parent of a lvol, making the lvol a"
                                   " clone of this external snapshot (see bdev_lvol_clone_bdev). The"
                                   " previous parent of the lvol, if any, can be another external"
                                   " snapshot or a snapshot; if the lvol is not a clone, it must be"
                                   " thin-provisioned. The size of the external snapshot device must be"
                                   " an integer multiple of cluster size of lvol's lvolstore.")
    p.add_argument('lvol_name', help='UUID or alias of the lvol to set external parent of')
    p.add_argument('parent_name', help='UUID or name of the external snapshot to become parent of lvol')
    p.set_defaults(func=bdev_lvol_set_parent_bdev)

    def bdev_lvol_delete_lvstore(args):
        args.client.bdev_lvol_delete_lvstore(
                                          uuid=args.uuid,
                                          lvs_name=args.lvs_name)

    p = subparsers.add_parser('bdev_lvol_delete_lvstore', help="Destroy a logical volume store.")
    p.add_argument('-u', '--uuid', help='UUID of the logical volume store to destroy')
    p.add_argument('-l', '--lvs-name', help='Name of the logical volume store to destroy')
    p.set_defaults(func=bdev_lvol_delete_lvstore)

    def bdev_lvol_get_lvstores(args):
        print_dict(args.client.bdev_lvol_get_lvstores(
                                                   uuid=args.uuid,
                                                   lvs_name=args.lvs_name))

    p = subparsers.add_parser('bdev_lvol_get_lvstores', help="Get a list of logical volume stores.")
    p.add_argument('-u', '--uuid', help='UUID of the logical volume store to retrieve information about')
    p.add_argument('-l', '--lvs-name', help='Name of the logical volume store to retrieve information about')
    p.set_defaults(func=bdev_lvol_get_lvstores)

    def bdev_lvol_get_lvols(args):
        print_dict(args.client.bdev_lvol_get_lvols(
                                                lvs_uuid=args.lvs_uuid,
                                                lvs_name=args.lvs_name))

    p = subparsers.add_parser('bdev_lvol_get_lvols',
                              help="Get a list of logical volumes. This list can be limited by lvol store"
                                   " and will display volumes even if they are degraded. Degraded lvols"
                                   " do not have an associated bdev, thus this RPC call may return lvols"
                                   " not returned by `bdev_get_bdevs`.")
    p.add_argument('-u', '--lvs-uuid', help='Only show volumes in the logical volume store with this UUID')
    p.add_argument('-l', '--lvs-name', help='Only show volumes in the logical volume store with this name')
    p.set_defaults(func=bdev_lvol_get_lvols)
