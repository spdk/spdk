def start_nbd_disk(client, bdev_name, nbd_device):
    params = {
        'bdev_name': bdev_name
    }
    if nbd_device:
        params['nbd_device'] = nbd_device
    return client.call('start_nbd_disk', params)


def stop_nbd_disk(client, nbd_device):
    params = {'nbd_device': nbd_device}
    return client.call('stop_nbd_disk', params)


def get_nbd_disks(client, nbd_device=None):
    params = {}
    if nbd_device:
        params['nbd_device'] = nbd_device
    return client.call('get_nbd_disks', params)
