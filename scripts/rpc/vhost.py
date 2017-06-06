from client import print_dict, print_array, int_arg


def get_vhost_scsi_controllers(args):
    print_dict(args.client.call(
        'get_vhost_scsi_controllers', verbose=args.verbose))


def construct_vhost_scsi_controller(args):
    params = {'ctrlr': args.ctrlr}

    if args.cpumask:
        params['cpumask'] = args.cpumask

    args.client.call('construct_vhost_scsi_controller',
                     params, verbose=args.verbose)


def remove_vhost_scsi_controller(args):
    params = {'ctrlr': args.ctrlr}
    args.client.call('remove_vhost_scsi_controller',
                     params, verbose=args.verbose)


def add_vhost_scsi_lun(args):
    params = {
        'ctrlr': args.ctrlr,
        'scsi_dev_num': args.scsi_dev_num,
        'lun_name': args.lun_name
    }
    args.client.call('add_vhost_scsi_lun', params, verbose=args.verbose)


def remove_vhost_scsi_dev(args):
    params = {
        'ctrlr': args.ctrlr,
        'scsi_dev_num': args.scsi_dev_num,
    }
    args.client.call('remove_vhost_scsi_dev', params, verbose=args.verbose)
