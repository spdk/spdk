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
