def make_bdev_blobfs(client, bdev_name, cluster_sz=None, force=None):
    """Build blobfs on bdev.
    Args:
    bdev_name: block device name to build blobfs
    cluster_sz: Size of cluster in bytes. Must be multiple of 4KiB page size
    force: Build new blobfs even one blobfs is already existed
    """
    params = {
        'bdev_name': bdev_name
    }
    if cluster_sz:
        params['cluster_sz'] = cluster_sz
    if force:
        params['force'] = force
    return client.call('make_bdev_blobfs', params)


def check_bdev_blobfs(client, bdev_name):
    """Check whether a blobfs is existed on bdev.
    Args:
    bdev_name: block device name to check blobfs
    """
    params = {
        'bdev_name': bdev_name
    }
    return client.call('check_bdev_blobfs', params)
