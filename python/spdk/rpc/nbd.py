#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2017 Intel Corporation.
#  All rights reserved.

from spdk.rpc.helpers import deprecated_method


@deprecated_method
def nbd_start_disk(client, bdev_name, nbd_device):
    params = {
        'bdev_name': bdev_name
    }
    if nbd_device:
        params['nbd_device'] = nbd_device
    return client.call('nbd_start_disk', params)


@deprecated_method
def nbd_stop_disk(client, nbd_device):
    params = {'nbd_device': nbd_device}
    return client.call('nbd_stop_disk', params)


@deprecated_method
def nbd_get_disks(client, nbd_device=None):
    params = {}
    if nbd_device:
        params['nbd_device'] = nbd_device
    return client.call('nbd_get_disks', params)
