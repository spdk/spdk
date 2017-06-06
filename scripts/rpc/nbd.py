from client import print_dict, print_array, int_arg


def start_nbd_disk(args):
    params = {
        'bdev_name': args.bdev_name,
        'nbd_device': args.nbd_device
    }
    args.client.call('start_nbd_disk', params)


def stop_nbd_disk(args):
    params = {'nbd_device': args.nbd_device}
    args.client.call('stop_nbd_disk', params)


def get_nbd_disks(args):
    params = {}
    if args.nbd_device:
        params['nbd_device'] = args.nbd_device
    print_dict(args.client.call('get_nbd_disks', params))
