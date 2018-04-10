def get_subsystems(client):
    return client.call('get_subsystems')


def get_subsystem_config(client, name):
    params = {'name': name}
    return client.call('get_subsystem_config', params)


def clear_subsystem(client, args):
    config = client.call('get_subsystem_config', {"name": args.subsystem})
    if config is None:
        return
    if args.verbose:
        print "Calling clear_%s_subsystem" % args.subsystem
    globals()["clear_%s_subsystem" % args.subsystem](args, config)


def clear_bdev_subsystem(args, bdev_config):
    rpc_bdevs = args.client.call("get_bdevs")
    for bdev in reversed(bdev_config):
        bdev_name = None
        if 'params' in bdev and 'name' in bdev['params']:
            bdev_name = bdev['params']['name']
        if 'method' in bdev:
            if bdev['method'] == 'construct_nvme_bdev':
                for rpc_bdev in rpc_bdevs:
                    if bdev_name in rpc_bdev['name'] and rpc_bdev['product_name'] == "NVMe disk":
                        args.client.call("delete_bdev", {'name': "%s" % rpc_bdev['name']})
            if bdev['method'] in ['construct_pmem_bdev', 'construct_rbd_bdev',
                                  'construct_malloc_bdev', 'construct_null_bdev',
                                  'construct_aio_bdev']:
                args.client.call("delete_bdev", {'name': bdev_name})
            elif bdev['method'] == 'construct_split_vbdev':
                args.client.call("destruct_split_vbdev", {'base_bdev': bdev['params']['base_bdev']})
            elif bdev['method'] == 'construct_virtio_dev':
                if bdev['params']['dev_type'] == 'blk':
                    args.client.call("delete_bdev", {'name': bdev_name})
                else:
                    args.client.call("remove_virtio_scsi_bdev", {'name': bdev_name})
        elif bdev_name:
            args.client.call("delete_bdev", {'name': bdev_name})


def clear_nbd_subsystem(args, nbd_config):
    pass


def clear_vhost_subsystem(args, vhost_config):
    pass


def clear_scsi_subsystem(args, scsi_config):
    pass


def clear_copy_subsystem(args, copy_config):
    pass


def clear_interface_subsystem(args, interface_config):
    pass


def clear_net_framework_subsystem(args, net_framework_config):
    pass
