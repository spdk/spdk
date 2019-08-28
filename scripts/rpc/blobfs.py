def blobfs_detect(client, bdev_name):
    """Detect whether a blobfs exists on bdev.

    Args:
        bdev_name: block device name to detect blobfs

    Returns:
        True if a blobfs exists on the bdev; False otherwise.
    """
    params = {
        'bdev_name': bdev_name
    }
    return client.call('blobfs_detect', params)


def blobfs_create(client, bdev_name, cluster_sz=None):
    """Build blobfs on bdev.

    Args:
        bdev_name: block device name to build blobfs
        cluster_sz: Size of cluster in bytes. Must be multiple of 4KiB page size (Optional)
    """
    params = {
        'bdev_name': bdev_name
    }
    if cluster_sz:
        params['cluster_sz'] = cluster_sz
    return client.call('blobfs_create', params)
