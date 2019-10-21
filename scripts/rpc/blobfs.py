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
        cluster_sz: Size of cluster in bytes (Optional). Must be multiple of 4KB page size. Default and minimal value is 1M.
    """
    params = {
        'bdev_name': bdev_name
    }
    if cluster_sz:
        params['cluster_sz'] = cluster_sz
    return client.call('blobfs_create', params)


def blobfs_mount(client, bdev_name, mountpoint):
    """Mount blobfs on bdev by FUSE.

    Args:
        bdev_name: block device name where the blobfs is
        mountpoint: Mountpoint path in host to mount blobfs
    """
    params = {
        'bdev_name': bdev_name,
        'mountpoint': mountpoint
    }
    return client.call('blobfs_mount', params)


def blobfs_set_cache_size(client, size_in_mb):
    """Set cache size for the blobstore filesystem.

    Args:
        size_in_mb: Cache size in megabytes

    Returns:
        True if cache size is set successfully; False if failed to set.
    """
    params = {
        'size_in_mb': size_in_mb
    }
    return client.call('blobfs_set_cache_size', params)
