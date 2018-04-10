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
    for bdev in reversed(bdev_config):
        if 'name' in bdev:
            args.client.call("delete_bdev", {'name': bdev['name']})
        elif 'method' in bdev and bdev['method'] == 'construct_nvme_bdev':
            args.client.call("delete_bdev", {'name': "%sn1" % bdev['params']['name']})
        elif 'method' in bdev and bdev['method'] == 'construct_split_vbdev':
            args.client.call("destruct_split_vbdev", {'base_bdev': bdev['params']['base_bdev']})
        elif 'method' in bdev and bdev['method'] == 'construct_pmem_bdev':
            args.client.call("delete_bdev", {'name': bdev['params']['name']})
        elif 'method' in bdev and bdev['method'] == 'construct_rbd_bdev':
            args.client.call("delete_bdev", {'name': bdev['params']['name']})
        elif 'method' in bdev and bdev['method'] == 'construct_virtio_dev':
            if bdev['params']['dev_type'] == 'blk':
                args.client.call("delete_bdev", {'name': bdev['params']['name']})
            else:
                args.client.call("remove_virtio_scsi_bdev", {'name': bdev['params']['name']})
        elif 'params' in bdev and 'name' in bdev['params']:
            args.client.call("delete_bdev", {'name': bdev['params']['name']})


def clear_nbd_subsystem(args, nbd_config):
    for nbd in nbd_config:
        if 'name' in nbd:
            args.client.call("stop_nbd_disk", {'name': nbd['name']})


def clear_vhost_subsystem(args, vhost_config):
    for vhost in reversed(vhost_config):
        if 'method' in vhost and vhost['method'] in 'construct_vhost_scsi_controller':
            args.client.call("remove_vhost_controller", {'ctrlr': vhost['params']['ctrlr']})
        elif 'method' in vhost and vhost['method'] in 'construct_vhost_blk_controller':
            args.client.call("remove_vhost_controller", {'ctrlr': vhost['params']['ctrlr']})


def clear_scsi_subsystem(args, scsi_config):
    pass


def clear_copy_subsystem(args, copy_config):
    pass


def clear_interface_subsystem(args, interface_config):
    pass


def clear_net_framework_subsystem(args, net_framework_config):
    pass
