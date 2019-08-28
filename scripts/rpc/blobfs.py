def bdev_blobfs_detect(client, bdev_name):
    """Detect whether a blobfs exists on bdev.

    Args:
        bdev_name: block device name to detect blobfs

    Returns:
        True if a blobfs exists on the bdev; False otherwise.
    """
    params = {
        'bdev_name': bdev_name
    }
    return client.call('bdev_blobfs_detect', params)
