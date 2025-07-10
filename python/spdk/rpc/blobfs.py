#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2019 Intel Corporation.
#  All rights reserved.


def blobfs_detect(client, bdev_name):
    """Detect whether a blobfs exists on bdev.

    Args:
        bdev_name: Block device name to detect blobfs

    Returns:
        result: True if a blobfs exists on the bdev; False otherwise.

    """
    params = dict()
    params['bdev_name'] = bdev_name
    return client.call('blobfs_detect', params)


def blobfs_create(client, bdev_name, cluster_sz=None):
    """Build blobfs on bdev.

    Args:
        bdev_name: Block device name to create blobfs
        cluster_sz: Size of cluster in bytes. Must be multiple of 4KiB page size, default and minimal value is 1M.

    Returns:
        None
    """
    params = dict()
    params['bdev_name'] = bdev_name
    if cluster_sz is not None:
        params['cluster_sz'] = cluster_sz
    return client.call('blobfs_create', params)


def blobfs_mount(client, bdev_name, mountpoint):
    """Mount a blobfs on bdev to one host path through FUSE

    Args:
        bdev_name: Block device name where the blobfs is
        mountpoint: Mountpoint path in host to mount blobfs

    Returns:
        None
    """
    params = dict()
    params['bdev_name'] = bdev_name
    params['mountpoint'] = mountpoint
    return client.call('blobfs_mount', params)


def blobfs_set_cache_size(client, size_in_mb):
    """Set cache pool size for blobfs filesystems.

    This RPC is only permitted when the cache pool is not already initialized.
    The cache pool is initialized when the first blobfs filesystem is initialized or loaded.
    It is freed when the all initialized or loaded filesystems are unloaded.

    Args:
        size_in_mb: Cache size in megabytes

    Returns:
        result: True if cache size is set successfully; False if failed to set.

    """
    params = dict()
    params['size_in_mb'] = size_in_mb
    return client.call('blobfs_set_cache_size', params)
