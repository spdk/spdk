def set_vhost_controller_coalescing(client, args):
    params = {
        'ctrlr': args.ctrlr,
        'delay_base_us': args.delay_base_us,
        'iops_threshold': args.iops_threshold,
    }
    return client.call('set_vhost_controller_coalescing', params)


def construct_vhost_scsi_controller(client, args):
    params = {'ctrlr': args.ctrlr}

    if args.cpumask:
        params['cpumask'] = args.cpumask

    return client.call('construct_vhost_scsi_controller', params)


def add_vhost_scsi_lun(client, args):
    params = {
        'ctrlr': args.ctrlr,
        'bdev_name': args.bdev_name,
        'scsi_target_num': args.scsi_target_num
    }
    return client.call('add_vhost_scsi_lun', params)


def remove_vhost_scsi_target(client, args):
    params = {
        'ctrlr': args.ctrlr,
        'scsi_target_num': args.scsi_target_num
    }
    return client.call('remove_vhost_scsi_target', params)


def construct_vhost_nvme_controller(client, args):
    params = {
        'ctrlr': args.ctrlr,
        'io_queues': args.io_queues
    }

    if args.cpumask:
        params['cpumask'] = args.cpumask

    return client.call('construct_vhost_nvme_controller', params)


def add_vhost_nvme_ns(client, args):
    params = {
        'ctrlr': args.ctrlr,
        'bdev_name': args.bdev_name,
    }

    return client.call('add_vhost_nvme_ns', params)


def construct_vhost_blk_controller(client, args):
    params = {
        'ctrlr': args.ctrlr,
        'dev_name': args.dev_name,
    }
    if args.cpumask:
        params['cpumask'] = args.cpumask
    if args.readonly:
        params['readonly'] = args.readonly
    return client.call('construct_vhost_blk_controller', params)


def get_vhost_controllers(client, args):
    return client.call('get_vhost_controllers')


def remove_vhost_controller(client, args):
    params = {'ctrlr': args.ctrlr}
    return client.call('remove_vhost_controller', params)


def construct_virtio_dev(client, args):
    params = {
        'name': args.name,
        'trtype': args.trtype,
        'traddr': args.traddr,
        'dev_type': args.dev_type
    }
    if args.vq_count:
        params['vq_count'] = args.vq_count
    if args.vq_size:
        params['vq_size'] = args.vq_size
    return client.call('construct_virtio_dev', params)


def construct_virtio_user_scsi_bdev(client, args):
    params = {
        'path': args.path,
        'name': args.name,
    }
    if args.vq_count:
        params['vq_count'] = args.vq_count
    if args.vq_size:
        params['vq_size'] = args.vq_size
    return client.call('construct_virtio_user_scsi_bdev', params)


def construct_virtio_pci_scsi_bdev(client, args):
    params = {
        'pci_address': args.pci_address,
        'name': args.name,
    }
    return client.call('construct_virtio_pci_scsi_bdev', params)


def remove_virtio_scsi_bdev(client, args):
    params = {'name': args.name}
    return client.call('remove_virtio_scsi_bdev', params)


def get_virtio_scsi_devs(client, args):
    return client.call('get_virtio_scsi_devs')


def construct_virtio_user_blk_bdev(client, args):
    params = {
        'path': args.path,
        'name': args.name,
    }
    if args.vq_count:
        params['vq_count'] = args.vq_count
    if args.vq_size:
        params['vq_size'] = args.vq_size
    return client.call('construct_virtio_user_blk_bdev', params)


def construct_virtio_pci_blk_bdev(client, args):
    params = {
        'pci_address': args.pci_address,
        'name': args.name,
    }
    return client.call('construct_virtio_pci_blk_bdev', params)
