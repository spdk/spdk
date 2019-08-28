def bdev_blobfs_check(client, bdev_name):
    """Check whether a blobfs is existed on bdev.
    Args:
    bdev_name: block device name to check blobfs
    """
    params = {
        'bdev_name': bdev_name
    }
    return client.call('bdev_blobfs_check', params)
