#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2022 Intel Corporation.
#  All rights reserved.


def ublk_create_target(client, cpumask=None, disable_user_copy=None):
    params = {}
    if cpumask:
        params['cpumask'] = cpumask
    if disable_user_copy:
        params['disable_user_copy'] = True
    return client.call('ublk_create_target', params)


def ublk_destroy_target(client):
    return client.call('ublk_destroy_target')


def ublk_start_disk(client, bdev_name, ublk_id=1, num_queues=1, queue_depth=128):
    params = {
        'bdev_name': bdev_name,
        'ublk_id': ublk_id
    }
    if num_queues:
        params['num_queues'] = num_queues
    if queue_depth:
        params['queue_depth'] = queue_depth
    return client.call('ublk_start_disk', params)


def ublk_stop_disk(client, ublk_id=1):
    params = {'ublk_id': ublk_id}
    return client.call('ublk_stop_disk', params)


def ublk_recover_disk(client, bdev_name, ublk_id):
    params = {
        'bdev_name': bdev_name,
        'ublk_id': ublk_id
    }
    return client.call('ublk_recover_disk', params)


def ublk_get_disks(client, ublk_id=1):
    params = {}
    if ublk_id is not None:
        params['ublk_id'] = ublk_id
    return client.call('ublk_get_disks', params)
