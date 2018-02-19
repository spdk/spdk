from client import print_dict, print_array, int_arg


def construct_lvol_store(args):
    params = {'bdev_name': args.bdev_name, 'lvs_name': args.lvs_name}
    if args.cluster_sz:
        params['cluster_sz'] = args.cluster_sz
    print_array(args.client.call('construct_lvol_store', params))


def construct_lvol_bdev(args):
    num_bytes = (args.size * 1024 * 1024)
    params = {'lvol_name': args.lvol_name, 'size': num_bytes}
    if args.thin_provision:
        params['thin_provision'] = args.thin_provision
    if (args.uuid and args.lvs_name) or (not args.uuid and not args.lvs_name):
        print("You need to specify either uuid or name of lvolstore")
    else:
        if args.uuid:
            params['uuid'] = args.uuid
        if args.lvs_name:
            params['lvs_name'] = args.lvs_name
        print_array(args.client.call('construct_lvol_bdev', params))


def snapshot_lvol_bdev(args):
    params = {'lvol_name': args.lvol_name, 'snapshot_name': args.snapshot_name}
    print_array(args.client.call('snapshot_lvol_bdev', params))


def clone_lvol_bdev(args):
    params = {'snapshot_name': args.snapshot_name, 'clone_name': args.clone_name}
    print_array(args.client.call('clone_lvol_bdev', params))


# Logical volume resize feature is disabled, as it is currently work in progress
#
# def resize_lvol_bdev(args):
#     params = {
#         'name': args.name,
#         'size': args.size,
#     }
#     args.client.call('resize_lvol_bdev', params)


def destroy_lvol_store(args):
    params = {}
    if (args.uuid and args.lvs_name) or (not args.uuid and not args.lvs_name):
        print("You need to specify either uuid or name of lvolstore")
    else:
        if args.uuid:
            params['uuid'] = args.uuid
        if args.lvs_name:
            params['lvs_name'] = args.lvs_name
        args.client.call('destroy_lvol_store', params)


def get_lvol_stores(args):
    params = {}
    if (args.uuid and args.lvs_name):
        print("You can only specify either uuid or name of lvolstore")
    if args.uuid:
        params['uuid'] = args.uuid
    if args.lvs_name:
        params['lvs_name'] = args.lvs_name
    print_dict(args.client.call('get_lvol_stores', params))
