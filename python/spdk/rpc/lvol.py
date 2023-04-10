#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2017 Intel Corporation.
#  All rights reserved.
#  Copyright (c) 2022-2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.


def bdev_lvol_create_lvstore(client, bdev_name, lvs_name, cluster_sz=None,
                             clear_method=None, num_md_pages_per_cluster_ratio=None):
    """Construct a logical volume store.

    Args:
        bdev_name: bdev on which to construct logical volume store
        lvs_name: name of the logical volume store to create
        cluster_sz: cluster size of the logical volume store in bytes (optional)
        clear_method: Change clear method for data region. Available: none, unmap, write_zeroes (optional)
        num_md_pages_per_cluster_ratio: metadata pages per cluster (optional)

    Returns:
        UUID of created logical volume store.
    """
    params = {'bdev_name': bdev_name, 'lvs_name': lvs_name}
    if cluster_sz:
        params['cluster_sz'] = cluster_sz
    if clear_method:
        params['clear_method'] = clear_method
    if num_md_pages_per_cluster_ratio:
        params['num_md_pages_per_cluster_ratio'] = num_md_pages_per_cluster_ratio
    return client.call('bdev_lvol_create_lvstore', params)


def bdev_lvol_rename_lvstore(client, old_name, new_name):
    """Rename a logical volume store.

    Args:
        old_name: existing logical volume store name
        new_name: new logical volume store name
    """
    params = {
        'old_name': old_name,
        'new_name': new_name
    }
    return client.call('bdev_lvol_rename_lvstore', params)


def bdev_lvol_grow_lvstore(client, uuid=None, lvs_name=None):
    """Grow the logical volume store to fill the underlying bdev

    Args:
        uuid: UUID of logical volume store to resize (optional)
        lvs_name: name of logical volume store to resize (optional)
    """
    if (uuid and lvs_name):
        raise ValueError("Exactly one of uuid or lvs_name may be specified")
    params = {}
    if uuid:
        params['uuid'] = uuid
    if lvs_name:
        params['lvs_name'] = lvs_name
    return client.call('bdev_lvol_grow_lvstore', params)


def bdev_lvol_create(client, lvol_name, size_in_mib, thin_provision=False, uuid=None, lvs_name=None, clear_method=None):
    """Create a logical volume on a logical volume store.

    Args:
        lvol_name: name of logical volume to create
        size_in_mib: desired size of logical volume in MiB (will be rounded up to a multiple of cluster size)
        thin_provision: True to enable thin provisioning
        uuid: UUID of logical volume store to create logical volume on (optional)
        lvs_name: name of logical volume store to create logical volume on (optional)

    Either uuid or lvs_name must be specified, but not both.

    Returns:
        Name of created logical volume block device.
    """
    if (uuid and lvs_name) or (not uuid and not lvs_name):
        raise ValueError("Either uuid or lvs_name must be specified, but not both")

    params = {'lvol_name': lvol_name, 'size_in_mib': size_in_mib}
    if thin_provision:
        params['thin_provision'] = thin_provision
    if uuid:
        params['uuid'] = uuid
    if lvs_name:
        params['lvs_name'] = lvs_name
    if clear_method:
        params['clear_method'] = clear_method
    return client.call('bdev_lvol_create', params)


def bdev_lvol_snapshot(client, lvol_name, snapshot_name):
    """Capture a snapshot of the current state of a logical volume.

    Args:
        lvol_name: logical volume to create a snapshot from
        snapshot_name: name for the newly created snapshot

    Returns:
        Name of created logical volume snapshot.
    """
    params = {
        'lvol_name': lvol_name,
        'snapshot_name': snapshot_name
    }
    return client.call('bdev_lvol_snapshot', params)


def bdev_lvol_clone(client, snapshot_name, clone_name):
    """Create a logical volume based on a snapshot.

    Args:
        snapshot_name: snapshot to clone
        clone_name: name of logical volume to create

    Returns:
        Name of created logical volume clone.
    """
    params = {
        'snapshot_name': snapshot_name,
        'clone_name': clone_name
    }
    return client.call('bdev_lvol_clone', params)


def bdev_lvol_clone_bdev(client, bdev, lvs_name, clone_name):
    """Create a logical volume based on a snapshot.

    Regardless of whether the bdev is specified by name or UUID, the bdev UUID
    will be stored in the logical volume's metadata for use while the lvolstore
    is loading. For this reason, it is important that the bdev chosen has a
    static UUID.

    Args:
        bdev: bdev to clone; must not be an lvol in same lvstore as clone
        lvs_name: name of logical volume store to use
        clone_name: name of logical volume to create

    Returns:
        Name of created logical volume clone.
    """
    params = {
        'bdev': bdev,
        'lvs_name': lvs_name,
        'clone_name': clone_name
    }
    return client.call('bdev_lvol_clone_bdev', params)


def bdev_lvol_rename(client, old_name, new_name):
    """Rename a logical volume.

    Args:
        old_name: existing logical volume name
        new_name: new logical volume name
    """
    params = {
        'old_name': old_name,
        'new_name': new_name
    }
    return client.call('bdev_lvol_rename', params)


def bdev_lvol_resize(client, name, size_in_mib):
    """Resize a logical volume.

    Args:
        name: name of logical volume to resize
        size_in_mib: desired size of logical volume in MiB (will be rounded up to a multiple of cluster size)
    """
    params = {
        'name': name,
        'size_in_mib': size_in_mib,
    }
    return client.call('bdev_lvol_resize', params)


def bdev_lvol_set_read_only(client, name):
    """Mark logical volume as read only.

    Args:
        name: name of logical volume to set as read only
    """
    params = {
        'name': name,
    }
    return client.call('bdev_lvol_set_read_only', params)


def bdev_lvol_delete(client, name):
    """Destroy a logical volume.

    Args:
        name: name of logical volume to destroy
    """
    params = {
        'name': name,
    }
    return client.call('bdev_lvol_delete', params)


def bdev_lvol_inflate(client, name):
    """Inflate a logical volume.

    Args:
        name: name of logical volume to inflate
    """
    params = {
        'name': name,
    }
    return client.call('bdev_lvol_inflate', params)


def bdev_lvol_decouple_parent(client, name):
    """Decouple parent of a logical volume.

    Args:
        name: name of logical volume to decouple parent
    """
    params = {
        'name': name,
    }
    return client.call('bdev_lvol_decouple_parent', params)


def bdev_lvol_delete_lvstore(client, uuid=None, lvs_name=None):
    """Destroy a logical volume store.

    Args:
        uuid: UUID of logical volume store to destroy (optional)
        lvs_name: name of logical volume store to destroy (optional)

    Either uuid or lvs_name must be specified, but not both.
    """
    if (uuid and lvs_name) or (not uuid and not lvs_name):
        raise ValueError("Exactly one of uuid or lvs_name must be specified")

    params = {}
    if uuid:
        params['uuid'] = uuid
    if lvs_name:
        params['lvs_name'] = lvs_name
    return client.call('bdev_lvol_delete_lvstore', params)


def bdev_lvol_get_lvstores(client, uuid=None, lvs_name=None):
    """List logical volume stores.

    Args:
        uuid: UUID of logical volume store to retrieve information about (optional)
        lvs_name: name of logical volume store to retrieve information about (optional)

    Either uuid or lvs_name may be specified, but not both.
    If both uuid and lvs_name are omitted, information about all logical volume stores is returned.
    """
    if (uuid and lvs_name):
        raise ValueError("Exactly one of uuid or lvs_name may be specified")
    params = {}
    if uuid:
        params['uuid'] = uuid
    if lvs_name:
        params['lvs_name'] = lvs_name
    return client.call('bdev_lvol_get_lvstores', params)


def bdev_lvol_get_lvols(client, lvs_uuid=None, lvs_name=None):
    """List logical volumes

    Args:
        lvs_uuid: Only show volumes in the logical volume store with this UUID (optional)
        lvs_name: Only show volumes in the logical volume store with this name (optional)

    Either lvs_uuid or lvs_name may be specified, but not both.
    If both lvs_uuid and lvs_name are omitted, information about volumes in all
    logical volume stores is returned.
    """
    if (lvs_uuid and lvs_name):
        raise ValueError("Exactly one of uuid or lvs_name may be specified")
    params = {}
    if lvs_uuid:
        params['lvs_uuid'] = lvs_uuid
    if lvs_name:
        params['lvs_name'] = lvs_name
    return client.call('bdev_lvol_get_lvols', params)
