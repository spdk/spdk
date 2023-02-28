#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2022 Intel Corporation.
#  All rights reserved.


def vfu_tgt_set_base_path(client, path):
    """Set socket base path.

    Args:
        path: base path
    """
    params = {
            'path': path
    }

    return client.call('vfu_tgt_set_base_path', params)


def vfu_virtio_delete_endpoint(client, name):
    """Delete specified endpoint name.

    Args:
        name: endpoint name
    """
    params = {
            'name': name
    }

    return client.call('vfu_virtio_delete_endpoint', params)


def vfu_virtio_create_blk_endpoint(client, name, bdev_name, cpumask, num_queues, qsize, packed_ring):
    """Create virtio-blk endpoint.

    Args:
        name: endpoint name
        bdev_name: name of block device
        cpumask: CPU core mask
        num_queues: number of vrings
        qsize: number of element of each vring
        packed_ring: enable packed ring
    """
    params = {
            'name': name,
            'bdev_name': bdev_name
    }
    if cpumask:
        params['cpumask'] = cpumask
    if num_queues:
        params['num_queues'] = num_queues
    if qsize:
        params['qsize'] = qsize
    if packed_ring:
        params['packed_ring'] = packed_ring

    return client.call('vfu_virtio_create_blk_endpoint', params)


def vfu_virtio_scsi_add_target(client, name, scsi_target_num, bdev_name):
    """Attach a block device to the specified SCSI target.

    Args:
        name: endpoint name
        scsi_target_num: SCSI target number
        bdev_name: name of block device
    """
    params = {
            'name': name,
            'scsi_target_num': scsi_target_num,
            'bdev_name': bdev_name
    }

    return client.call('vfu_virtio_scsi_add_target', params)


def vfu_virtio_scsi_remove_target(client, name, scsi_target_num):
    """Remove specified SCSI target of socket endpoint.

    Args:
        name: endpoint name
        scsi_target_num: SCSI target number
    """
    params = {
            'name': name,
            'scsi_target_num': scsi_target_num
    }

    return client.call('vfu_virtio_scsi_remove_target', params)


def vfu_virtio_create_scsi_endpoint(client, name, cpumask, num_io_queues, qsize, packed_ring):
    """Create virtio-scsi endpoint.

    Args:
        name: endpoint name
        cpumask: CPU core mask
        num_io_queues: number of IO vrings
        qsize: number of element of each vring
        packed_ring: enable packed ring
    """
    params = {
            'name': name,
    }
    if cpumask:
        params['cpumask'] = cpumask
    if num_io_queues:
        params['num_io_queues'] = num_io_queues
    if qsize:
        params['qsize'] = qsize
    if packed_ring:
        params['packed_ring'] = packed_ring

    return client.call('vfu_virtio_create_scsi_endpoint', params)
