from .helpers import deprecated_alias


@deprecated_alias('construct_lvol_store')
def bdev_lvol_create_lvstore(client, bdev_name, lvs_name, cluster_sz=None, clear_method=None):
    """Construct a logical volume store.

    Args:
        bdev_name: bdev on which to construct logical volume store
        lvs_name: name of the logical volume store to create
        cluster_sz: cluster size of the logical volume store in bytes (optional)
        clear_method: Change clear method for data region. Available: none, unmap, write_zeroes (optional)

    Returns:
        UUID of created logical volume store.
    """
    params = {'bdev_name': bdev_name, 'lvs_name': lvs_name}
    if cluster_sz:
        params['cluster_sz'] = cluster_sz
    if clear_method:
        params['clear_method'] = clear_method
    return client.call('bdev_lvol_create_lvstore', params)


@deprecated_alias('rename_lvol_store')
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


@deprecated_alias('construct_lvol_bdev')
def bdev_lvol_create(client, lvol_name, size, thin_provision=False, uuid=None, lvs_name=None, clear_method=None):
    """Create a logical volume on a logical volume store.

    Args:
        lvol_name: name of logical volume to create
        size: desired size of logical volume in bytes (will be rounded up to a multiple of cluster size)
        thin_provision: True to enable thin provisioning
        uuid: UUID of logical volume store to create logical volume on (optional)
        lvs_name: name of logical volume store to create logical volume on (optional)

    Either uuid or lvs_name must be specified, but not both.

    Returns:
        Name of created logical volume block device.
    """
    if (uuid and lvs_name) or (not uuid and not lvs_name):
        raise ValueError("Either uuid or lvs_name must be specified, but not both")

    params = {'lvol_name': lvol_name, 'size': size}
    if thin_provision:
        params['thin_provision'] = thin_provision
    if uuid:
        params['uuid'] = uuid
    if lvs_name:
        params['lvs_name'] = lvs_name
    if clear_method:
        params['clear_method'] = clear_method
    return client.call('bdev_lvol_create', params)


@deprecated_alias('snapshot_lvol_bdev')
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


@deprecated_alias('clone_lvol_bdev')
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


@deprecated_alias('rename_lvol_bdev')
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


@deprecated_alias('resize_lvol_bdev')
def bdev_lvol_resize(client, name, size):
    """Resize a logical volume.

    Args:
        name: name of logical volume to resize
        size: desired size of logical volume in bytes (will be rounded up to a multiple of cluster size)
    """
    params = {
        'name': name,
        'size': size,
    }
    return client.call('bdev_lvol_resize', params)


@deprecated_alias('set_read_only_lvol_bdev')
def bdev_lvol_set_read_only(client, name):
    """Mark logical volume as read only.

    Args:
        name: name of logical volume to set as read only
    """
    params = {
        'name': name,
    }
    return client.call('bdev_lvol_set_read_only', params)


@deprecated_alias('destroy_lvol_bdev')
def bdev_lvol_delete(client, name):
    """Destroy a logical volume.

    Args:
        name: name of logical volume to destroy
    """
    params = {
        'name': name,
    }
    return client.call('bdev_lvol_delete', params)


@deprecated_alias('inflate_lvol_bdev')
def bdev_lvol_inflate(client, name):
    """Inflate a logical volume.

    Args:
        name: name of logical volume to inflate
    """
    params = {
        'name': name,
    }
    return client.call('bdev_lvol_inflate', params)


@deprecated_alias('decouple_parent_lvol_bdev')
def bdev_lvol_decouple_parent(client, name):
    """Decouple parent of a logical volume.

    Args:
        name: name of logical volume to decouple parent
    """
    params = {
        'name': name,
    }
    return client.call('bdev_lvol_decouple_parent', params)


@deprecated_alias('destroy_lvol_store')
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


@deprecated_alias('get_lvol_stores')
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
