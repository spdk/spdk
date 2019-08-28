def bdev_blobfs_check(client, bdev_name):
    """Check whether a blobfs exists on bdev.

    Args:
        bdev_name: block device name to check blobfs

    Returns:
        True for existence or False for inexistence
    """
    params = {
        'bdev_name': bdev_name
    }
    return client.call('bdev_blobfs_check', params)


def bdev_blobfs_create(client, bdev_name, cluster_sz=None):
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
    return client.call('bdev_blobfs_create', params)
