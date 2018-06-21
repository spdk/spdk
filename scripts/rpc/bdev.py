def set_bdev_options(client, bdev_io_pool_size=None, bdev_io_cache_size=None):
    """Set parameters for the bdev subsystem.

    Args:
        bdev_io_pool_size: number of bdev_io structures in shared buffer pool (optional)
        bdev_io_cache_size: maximum number of bdev_io structures cached per thread (optional)
    """
    params = {}

    if bdev_io_pool_size:
        params['bdev_io_pool_size'] = bdev_io_pool_size
    if bdev_io_cache_size:
        params['bdev_io_cache_size'] = bdev_io_cache_size

    return client.call('set_bdev_options', params)


def construct_malloc_bdev(client, num_blocks, block_size, name=None, uuid=None):
    """Construct a malloc block device.

    Args:
        num_blocks: size of block device in blocks
        block_size: block size of device; must be a power of 2 and at least 512
        name: name of block device (optional)
        uuid: UUID of block device (optional)

    Returns:
        Name of created block device.
    """
    params = {'num_blocks': num_blocks, 'block_size': block_size}
    if name:
        params['name'] = name
    if uuid:
        params['uuid'] = uuid
    return client.call('construct_malloc_bdev', params)


def delete_malloc_bdev(client, name):
    """Delete malloc block device.

    Args:
        bdev_name: name of malloc bdev to delete
    """
    params = {'name': name}
    return client.call('delete_malloc_bdev', params)


def construct_null_bdev(client, num_blocks, block_size, name, uuid=None):
    """Construct a null block device.

    Args:
        num_blocks: size of block device in blocks
        block_size: block size of device; must be a power of 2 and at least 512
        name: name of block device
        uuid: UUID of block device (optional)

    Returns:
        Name of created block device.
    """
    params = {'name': name, 'num_blocks': num_blocks,
              'block_size': block_size}
    if uuid:
        params['uuid'] = uuid
    return client.call('construct_null_bdev', params)


def construct_aio_bdev(client, filename, name, block_size=None):
    """Construct a Linux AIO block device.

    Args:
        filename: path to device or file (ex: /dev/sda)
        name: name of block device
        block_size: block size of device (optional; autodetected if omitted)

    Returns:
        Name of created block device.
    """
    params = {'name': name,
              'filename': filename}

    if block_size:
        params['block_size'] = block_size

    return client.call('construct_aio_bdev', params)


def delete_aio_bdev(client, name):
    """Remove aio bdev from the system.

    Args:
        bdev_name: name of aio bdev to delete
    """
    params = {'name': name}
    return client.call('delete_aio_bdev', params)


def construct_nvme_bdev(client, name, trtype, traddr, adrfam=None, trsvcid=None, subnqn=None):
    """Construct NVMe namespace block device.

    Args:
        name: bdev name prefix; "n" + namespace ID will be appended to create unique names
        trtype: transport type ("PCIe", "RDMA")
        traddr: transport address (PCI BDF or IP address)
        adrfam: address family ("IPv4", "IPv6", "IB", or "FC") (optional for PCIe)
        trsvcid: transport service ID (port number for IP-based addresses; optional for PCIe)
        subnqn: subsystem NQN to connect to (optional)

    Returns:
        Name of created block device.
    """
    params = {'name': name,
              'trtype': trtype,
              'traddr': traddr}

    if adrfam:
        params['adrfam'] = adrfam

    if trsvcid:
        params['trsvcid'] = trsvcid

    if subnqn:
        params['subnqn'] = subnqn

    return client.call('construct_nvme_bdev', params)


def construct_rbd_bdev(client, pool_name, rbd_name, block_size, name=None):
    """Construct a Ceph RBD block device.

    Args:
        pool_name: Ceph RBD pool name
        rbd_name: Ceph RBD image name
        block_size: block size of RBD volume
        name: name of block device (optional)

    Returns:
        Name of created block device.
    """
    params = {
        'pool_name': pool_name,
        'rbd_name': rbd_name,
        'block_size': block_size,
    }

    if name:
        params['name'] = name

    return client.call('construct_rbd_bdev', params)


def construct_error_bdev(client, base_name):
    """Construct an error injection block device.

    Args:
        base_name: base bdev name
    """
    params = {'base_name': base_name}
    return client.call('construct_error_bdev', params)


def delete_error_bdev(client, name):
    """Remove error bdev from the system.

    Args:
        bdev_name: name of error bdev to delete
    """
    params = {'name': name}
    return client.call('delete_error_bdev', params)


def construct_iscsi_bdev(client, name, url, initiator_iqn):
    """Construct a iSCSI block device.

    Args:
        name: name of block device
        url: iSCSI URL
        initiator_iqn: IQN name to be used by initiator

    Returns:
        Name of created block device.
    """
    params = {
        'name': name,
        'url': url,
        'initiator_iqn': initiator_iqn,
    }
    return client.call('construct_iscsi_bdev', params)


def delete_iscsi_bdev(client, name):
    """Remove iSCSI bdev from the system.

    Args:
        bdev_name: name of iSCSI bdev to delete
    """
    params = {'name': name}
    return client.call('delete_iscsi_bdev', params)


def construct_pmem_bdev(client, pmem_file, name):
    """Construct a libpmemblk block device.

    Args:
        pmem_file: path to pmemblk pool file
        name: name of block device

    Returns:
        Name of created block device.
    """
    params = {
        'pmem_file': pmem_file,
        'name': name
    }
    return client.call('construct_pmem_bdev', params)


def construct_passthru_bdev(client, base_bdev_name, passthru_bdev_name):
    """Construct a pass-through block device.

    Args:
        base_bdev_name: name of the existing bdev
        passthru_bdev_name: name of block device

    Returns:
        Name of created block device.
    """
    params = {
        'base_bdev_name': base_bdev_name,
        'passthru_bdev_name': passthru_bdev_name,
    }
    return client.call('construct_passthru_bdev', params)


def construct_split_vbdev(client, base_bdev, split_count, split_size_mb=None):
    """Construct split block devices from a base bdev.

    Args:
        base_bdev: name of bdev to split
        split_count: number of split bdevs to create
        split_size_mb: size of each split volume in MiB (optional)

    Returns:
        List of created block devices.
    """
    params = {
        'base_bdev': base_bdev,
        'split_count': split_count,
    }
    if split_size_mb:
        params['split_size_mb'] = split_size_mb

    return client.call('construct_split_vbdev', params)


def destruct_split_vbdev(client, base_bdev):
    """Destroy split block devices.

    Args:
        base_bdev: name of previously split bdev
    """
    params = {
        'base_bdev': base_bdev,
    }

    return client.call('destruct_split_vbdev', params)


def get_bdevs(client, name=None):
    """Get information about block devices.

    Args:
        name: bdev name to query (optional; if omitted, query all bdevs)

    Returns:
        List of bdev information objects.
    """
    params = {}
    if name:
        params['name'] = name
    return client.call('get_bdevs', params)


def get_bdevs_config(client, name=None):
    """Get configuration for block devices.

    Args:
        name: bdev name to query (optional; if omitted, query all bdevs)

    Returns:
        List of RPC methods to reproduce the current bdev configuration.
    """
    params = {}
    if name:
        params['name'] = name
    return client.call('get_bdevs_config', params)


def get_bdevs_iostat(client, name=None):
    """Get I/O statistics for block devices.

    Args:
        name: bdev name to query (optional; if omitted, query all bdevs)

    Returns:
        I/O statistics for the requested block devices.
    """
    params = {}
    if name:
        params['name'] = name
    return client.call('get_bdevs_iostat', params)


def delete_bdev(client, bdev_name):
    """Remove a bdev from the system.

    Args:
        bdev_name: name of bdev to delete
    """
    params = {'name': bdev_name}
    return client.call('delete_bdev', params)


def bdev_inject_error(client, name, io_type, error_type, num=1):
    """Inject an error via an error bdev.

    Args:
        name: name of error bdev
        io_type: one of "clear", "read", "write", "unmap", "flush", or "all"
        error_type: one of "failure" or "pending"
        num: number of commands to fail
    """
    params = {
        'name': name,
        'io_type': io_type,
        'error_type': error_type,
        'num': num,
    }

    return client.call('bdev_inject_error', params)


def set_bdev_qos_limit_iops(client, name, ios_per_sec):
    """Set QoS IOPS limit on a block device.

    Args:
        name: name of block device
        ios_per_sec: IOs per second limit (>=10000, example: 20000). 0 means unlimited.
    """
    params = {}
    params['name'] = name
    params['ios_per_sec'] = ios_per_sec
    return client.call('set_bdev_qos_limit_iops', params)


def apply_firmware(client, bdev_name, filename):
    """Download and commit firmware to NVMe device.

    Args:
        bdev_name: name of NVMe block device
        filename: filename of the firmware to download
    """
    params = {
        'filename': filename,
        'bdev_name': bdev_name,
    }
    return client.call('apply_nvme_firmware', params)
