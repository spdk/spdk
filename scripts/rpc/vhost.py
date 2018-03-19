def set_vhost_controller_coalescing(args):
    params = {
        'ctrlr': args.ctrlr,
        'delay_base_us': args.delay_base_us,
        'iops_threshold': args.iops_threshold,
    }
    return args.client.call('set_vhost_controller_coalescing', params)


def construct_vhost_scsi_controller(args):
    params = {'ctrlr': args.ctrlr}

    if args.cpumask:
        params['cpumask'] = args.cpumask

    return args.client.call('construct_vhost_scsi_controller', params)


def add_vhost_scsi_lun(args):
    params = {
        'ctrlr': args.ctrlr,
        'bdev_name': args.bdev_name,
        'scsi_target_num': args.scsi_target_num
    }
    return args.client.call('add_vhost_scsi_lun', params)


def remove_vhost_scsi_target(args):
    params = {
        'ctrlr': args.ctrlr,
        'scsi_target_num': args.scsi_target_num
    }
    return args.client.call('remove_vhost_scsi_target', params)


def construct_vhost_blk_controller(args):
    params = {
        'ctrlr': args.ctrlr,
        'dev_name': args.dev_name,
    }
    if args.cpumask:
        params['cpumask'] = args.cpumask
    if args.readonly:
        params['readonly'] = args.readonly
    return args.client.call('construct_vhost_blk_controller', params)


def get_vhost_controllers(args):
    return args.client.call('get_vhost_controllers')


def remove_vhost_controller(args):
    params = {'ctrlr': args.ctrlr}
    return args.client.call('remove_vhost_controller', params)


def construct_virtio_user_scsi_bdev(args):
    params = {
        'path': args.path,
        'name': args.name,
    }
    if args.vq_count:
        params['vq_count'] = args.vq_count
    if args.vq_size:
        params['vq_size'] = args.vq_size
    return args.client.call('construct_virtio_user_scsi_bdev', params)


def construct_virtio_pci_scsi_bdev(args):
    params = {
        'pci_address': args.pci_address,
        'name': args.name,
    }
    return args.client.call('construct_virtio_pci_scsi_bdev', params)


def remove_virtio_scsi_bdev(args):
    params = {'name': args.name}
    return args.client.call('remove_virtio_scsi_bdev', params)


def construct_virtio_user_blk_bdev(args):
    params = {
        'path': args.path,
        'name': args.name,
    }
    if args.vq_count:
        params['vq_count'] = args.vq_count
    if args.vq_size:
        params['vq_size'] = args.vq_size
    return args.client.call('construct_virtio_user_blk_bdev', params)


def construct_virtio_pci_blk_bdev(args):
    params = {
        'pci_address': args.pci_address,
        'name': args.name,
    }
    return args.client.call('construct_virtio_pci_blk_bdev', params)
