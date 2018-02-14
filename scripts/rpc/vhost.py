from client import print_dict, print_array, int_arg


def set_vhost_controller_coalescing(args):
    params = {
        'ctrlr': args.ctrlr,
        'delay_base_us': args.delay_base_us,
        'iops_threshold': args.iops_threshold,
    }
    args.client.call('set_vhost_controller_coalescing', params)


def construct_vhost_scsi_controller(args):
    params = {'ctrlr': args.ctrlr}

    if args.cpumask:
        params['cpumask'] = args.cpumask

    args.client.call('construct_vhost_scsi_controller',
                     params)


def add_vhost_scsi_lun(args):
    params = {
        'ctrlr': args.ctrlr,
        'bdev_name': args.bdev_name,
        'scsi_target_num': args.scsi_target_num
    }
    args.client.call('add_vhost_scsi_lun', params)


def remove_vhost_scsi_target(args):
    params = {
        'ctrlr': args.ctrlr,
        'scsi_target_num': args.scsi_target_num
    }
    args.client.call('remove_vhost_scsi_target', params)


def construct_vhost_blk_controller(args):
    params = {
        'ctrlr': args.ctrlr,
        'dev_name': args.dev_name,
    }
    if args.cpumask:
        params['cpumask'] = args.cpumask
    if args.readonly:
        params['readonly'] = args.readonly
    args.client.call('construct_vhost_blk_controller', params)


def get_vhost_controllers(args):
    print_dict(args.client.call('get_vhost_controllers'))


def remove_vhost_controller(args):
    params = {'ctrlr': args.ctrlr}
    args.client.call('remove_vhost_controller', params)


def construct_virtio_user_scsi_bdev(args):
    params = {
        'path': args.path,
        'name': args.name,
    }
    if args.vq_count:
        params['vq_count'] = args.vq_count
    if args.vq_size:
        params['vq_size'] = args.vq_size
    print_dict(args.client.call('construct_virtio_user_scsi_bdev', params))


def construct_virtio_pci_scsi_bdev(args):
    params = {
        'pci_address': args.pci_address,
        'name': args.name,
    }
    print_dict(args.client.call('construct_virtio_pci_scsi_bdev', params))


def remove_virtio_scsi_bdev(args):
    params = {'name': args.name}
    args.client.call('remove_virtio_scsi_bdev', params)
