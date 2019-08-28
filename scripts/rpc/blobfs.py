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
