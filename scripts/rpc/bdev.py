from .helpers import deprecated_alias


@deprecated_alias('set_bdev_options')
def bdev_set_options(client, bdev_io_pool_size=None, bdev_io_cache_size=None, bdev_auto_examine=None):
    """Set parameters for the bdev subsystem.

    Args:
        bdev_io_pool_size: number of bdev_io structures in shared buffer pool (optional)
        bdev_io_cache_size: maximum number of bdev_io structures cached per thread (optional)
        bdev_auto_examine: if set to false, the bdev layer will not examine every disks automatically (optional)
    """
    params = {}

    if bdev_io_pool_size:
        params['bdev_io_pool_size'] = bdev_io_pool_size
    if bdev_io_cache_size:
        params['bdev_io_cache_size'] = bdev_io_cache_size
    if bdev_auto_examine is not None:
        params["bdev_auto_examine"] = bdev_auto_examine

    return client.call('bdev_set_options', params)


def bdev_examine(client, name):
    """Examine a bdev manually. If the bdev does not exist yet when this RPC is called,
    it will be examined when it is created

    Args:
        name: name of the bdev
    """
    params = {
        'name': name
    }
    return client.call('bdev_examine', params)


@deprecated_alias('construct_compress_bdev')
def bdev_compress_create(client, base_bdev_name, pm_path, lb_size):
    """Construct a compress virtual block device.

    Args:
        base_bdev_name: name of the underlying base bdev
        pm_path: path to persistent memory
        lb_size: logical block size for the compressed vol in bytes.  Must be 4K or 512.

    Returns:
        Name of created virtual block device.
    """
    params = {'base_bdev_name': base_bdev_name, 'pm_path': pm_path, 'lb_size': lb_size}

    return client.call('bdev_compress_create', params)


@deprecated_alias('delete_compress_bdev')
def bdev_compress_delete(client, name):
    """Delete compress virtual block device.

    Args:
        name: name of compress vbdev to delete
    """
    params = {'name': name}
    return client.call('bdev_compress_delete', params)


@deprecated_alias('set_compress_pmd')
@deprecated_alias('compress_set_pmd')
def bdev_compress_set_pmd(client, pmd):
    """Set pmd options for the bdev compress.

    Args:
        pmd: 0 = auto-select, 1 = QAT, 2 = ISAL
    """
    params = {'pmd': pmd}

    return client.call('bdev_compress_set_pmd', params)


def bdev_compress_get_orphans(client, name=None):
    """Get a list of comp bdevs that do not have a pmem file (aka orphaned).

    Args:
        name: comp bdev name to query (optional; if omitted, query all comp bdevs)

    Returns:
        List of comp bdev names.
    """
    params = {}
    if name:
        params['name'] = name
    return client.call('bdev_compress_get_orphans', params)


@deprecated_alias('construct_crypto_bdev')
def bdev_crypto_create(client, base_bdev_name, name, crypto_pmd, key, cipher=None, key2=None):
    """Construct a crypto virtual block device.

    Args:
        base_bdev_name: name of the underlying base bdev
        name: name for the crypto vbdev
        crypto_pmd: name of of the DPDK crypto driver to use
        key: key

    Returns:
        Name of created virtual block device.
    """
    params = {'base_bdev_name': base_bdev_name, 'name': name, 'crypto_pmd': crypto_pmd, 'key': key}
    if cipher:
        params['cipher'] = cipher
    if key2:
        params['key2'] = key2
    return client.call('bdev_crypto_create', params)


@deprecated_alias('delete_crypto_bdev')
def bdev_crypto_delete(client, name):
    """Delete crypto virtual block device.

    Args:
        name: name of crypto vbdev to delete
    """
    params = {'name': name}
    return client.call('bdev_crypto_delete', params)


@deprecated_alias('construct_ocf_bdev')
def bdev_ocf_create(client, name, mode, cache_line_size, cache_bdev_name, core_bdev_name):
    """Add an OCF block device

    Args:
        name: name of constructed OCF bdev
        mode: OCF cache mode: {'wb', 'wt', 'pt', 'wa', 'wi', 'wo'}
        cache_line_size: OCF cache line size. The unit is KiB: {4, 8, 16, 32, 64}
        cache_bdev_name: name of underlying cache bdev
        core_bdev_name: name of underlying core bdev

    Returns:
        Name of created block device
    """
    params = {
        'name': name,
        'mode': mode,
        'cache_line_size': cache_line_size,
        'cache_bdev_name': cache_bdev_name,
        'core_bdev_name': core_bdev_name,
    }

    return client.call('bdev_ocf_create', params)


@deprecated_alias('delete_ocf_bdev')
def bdev_ocf_delete(client, name):
    """Delete an OCF device

    Args:
        name: name of OCF bdev

    """
    params = {'name': name}

    return client.call('bdev_ocf_delete', params)


@deprecated_alias('get_ocf_stats')
def bdev_ocf_get_stats(client, name):
    """Get statistics of chosen OCF block device

    Args:
        name: name of OCF bdev

    Returns:
        Statistics as json object
    """
    params = {'name': name}

    return client.call('bdev_ocf_get_stats', params)


@deprecated_alias('get_ocf_stats')
def bdev_ocf_get_bdevs(client, name=None):
    """Get list of OCF devices including unregistered ones

    Args:
        name: name of OCF vbdev or name of cache device or name of core device (optional)

    Returns:
        Array of OCF devices with their current status
    """
    params = None
    if name:
        params = {'name': name}
    return client.call('bdev_ocf_get_bdevs', params)


@deprecated_alias('construct_malloc_bdev')
def bdev_malloc_create(client, num_blocks, block_size, name=None, uuid=None):
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
    return client.call('bdev_malloc_create', params)


@deprecated_alias('delete_malloc_bdev')
def bdev_malloc_delete(client, name):
    """Delete malloc block device.

    Args:
        bdev_name: name of malloc bdev to delete
    """
    params = {'name': name}
    return client.call('bdev_malloc_delete', params)


@deprecated_alias('construct_null_bdev')
def bdev_null_create(client, num_blocks, block_size, name, uuid=None, md_size=None,
                     dif_type=None, dif_is_head_of_md=None):
    """Construct a null block device.

    Args:
        num_blocks: size of block device in blocks
        block_size: block size of device; data part size must be a power of 2 and at least 512
        name: name of block device
        uuid: UUID of block device (optional)
        md_size: metadata size of device (optional)
        dif_type: protection information type (optional)
        dif_is_head_of_md: protection information is in the first 8 bytes of metadata (optional)

    Returns:
        Name of created block device.
    """
    params = {'name': name, 'num_blocks': num_blocks,
              'block_size': block_size}
    if uuid:
        params['uuid'] = uuid
    if md_size:
        params['md_size'] = md_size
    if dif_type:
        params['dif_type'] = dif_type
    if dif_is_head_of_md:
        params['dif_is_head_of_md'] = dif_is_head_of_md
    return client.call('bdev_null_create', params)


@deprecated_alias('delete_null_bdev')
def bdev_null_delete(client, name):
    """Remove null bdev from the system.

    Args:
        name: name of null bdev to delete
    """
    params = {'name': name}
    return client.call('bdev_null_delete', params)


@deprecated_alias('get_raid_bdevs')
def bdev_raid_get_bdevs(client, category):
    """Get list of raid bdevs based on category

    Args:
        category: any one of all or online or configuring or offline

    Returns:
        List of raid bdev names
    """
    params = {'category': category}
    return client.call('bdev_raid_get_bdevs', params)


@deprecated_alias('construct_raid_bdev')
def bdev_raid_create(client, name, raid_level, base_bdevs, strip_size=None, strip_size_kb=None):
    """Create raid bdev. Either strip size arg will work but one is required.

    Args:
        name: user defined raid bdev name
        strip_size (deprecated): strip size of raid bdev in KB, supported values like 8, 16, 32, 64, 128, 256, etc
        strip_size_kb: strip size of raid bdev in KB, supported values like 8, 16, 32, 64, 128, 256, etc
        raid_level: raid level of raid bdev, supported values 0
        base_bdevs: Space separated names of Nvme bdevs in double quotes, like "Nvme0n1 Nvme1n1 Nvme2n1"

    Returns:
        None
    """
    params = {'name': name, 'raid_level': raid_level, 'base_bdevs': base_bdevs}

    if strip_size:
        params['strip_size'] = strip_size

    if strip_size_kb:
        params['strip_size_kb'] = strip_size_kb

    return client.call('bdev_raid_create', params)


@deprecated_alias('destroy_raid_bdev')
def bdev_raid_delete(client, name):
    """Delete raid bdev

    Args:
        name: raid bdev name

    Returns:
        None
    """
    params = {'name': name}
    return client.call('bdev_raid_delete', params)


@deprecated_alias('construct_aio_bdev')
def bdev_aio_create(client, filename, name, block_size=None):
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

    return client.call('bdev_aio_create', params)


@deprecated_alias('delete_aio_bdev')
def bdev_aio_delete(client, name):
    """Remove aio bdev from the system.

    Args:
        bdev_name: name of aio bdev to delete
    """
    params = {'name': name}
    return client.call('bdev_aio_delete', params)


def bdev_uring_create(client, filename, name, block_size=None):
    """Create a bdev with Linux io_uring backend.

    Args:
        filename: path to device or file (ex: /dev/nvme0n1)
        name: name of bdev
        block_size: block size of device (optional; autodetected if omitted)

    Returns:
        Name of created bdev.
    """
    params = {'name': name,
              'filename': filename}

    if block_size:
        params['block_size'] = block_size

    return client.call('bdev_uring_create', params)


def bdev_uring_delete(client, name):
    """Delete a uring bdev.

    Args:
        name: name of uring bdev to delete
    """
    params = {'name': name}
    return client.call('bdev_uring_delete', params)


@deprecated_alias('set_bdev_nvme_options')
def bdev_nvme_set_options(client, action_on_timeout=None, timeout_us=None, retry_count=None,
                          arbitration_burst=None, low_priority_weight=None,
                          medium_priority_weight=None, high_priority_weight=None,
                          nvme_adminq_poll_period_us=None, nvme_ioq_poll_period_us=None, io_queue_requests=None,
                          delay_cmd_submit=None):
    """Set options for the bdev nvme. This is startup command.

    Args:
        action_on_timeout:  action to take on command time out. Valid values are: none, reset, abort (optional)
        timeout_us: Timeout for each command, in microseconds. If 0, don't track timeouts (optional)
        retry_count: The number of attempts per I/O when an I/O fails (optional)
        arbitration_burst: The value is expressed as a power of two (optional)
        low_prioity_weight: The number of commands that may be executed from the low priority queue at one time (optional)
        medium_prioity_weight: The number of commands that may be executed from the medium priority queue at one time (optional)
        high_prioity_weight: The number of commands that may be executed from the high priority queue at one time (optional)
        nvme_adminq_poll_period_us: How often the admin queue is polled for asynchronous events in microseconds (optional)
        nvme_ioq_poll_period_us: How often to poll I/O queues for completions in microseconds (optional)
        io_queue_requests: The number of requests allocated for each NVMe I/O queue. Default: 512 (optional)
        delay_cmd_submit: Enable delayed NVMe command submission to allow batching of multiple commands (optional)
    """
    params = {}

    if action_on_timeout:
        params['action_on_timeout'] = action_on_timeout

    if timeout_us is not None:
        params['timeout_us'] = timeout_us

    if retry_count is not None:
        params['retry_count'] = retry_count

    if arbitration_burst is not None:
        params['arbitration_burst'] = arbitration_burst

    if low_priority_weight is not None:
        params['low_priority_weight'] = low_priority_weight

    if medium_priority_weight is not None:
        params['medium_priority_weight'] = medium_priority_weight

    if high_priority_weight is not None:
        params['high_priority_weight'] = high_priority_weight

    if nvme_adminq_poll_period_us:
        params['nvme_adminq_poll_period_us'] = nvme_adminq_poll_period_us

    if nvme_ioq_poll_period_us is not None:
        params['nvme_ioq_poll_period_us'] = nvme_ioq_poll_period_us

    if io_queue_requests is not None:
        params['io_queue_requests'] = io_queue_requests

    if delay_cmd_submit is not None:
        params['delay_cmd_submit'] = delay_cmd_submit

    return client.call('bdev_nvme_set_options', params)


@deprecated_alias('set_bdev_nvme_hotplug')
def bdev_nvme_set_hotplug(client, enable, period_us=None):
    """Set options for the bdev nvme. This is startup command.

    Args:
       enable: True to enable hotplug, False to disable.
       period_us: how often the hotplug is processed for insert and remove events. Set 0 to reset to default. (optional)
    """
    params = {'enable': enable}

    if period_us:
        params['period_us'] = period_us

    return client.call('bdev_nvme_set_hotplug', params)


@deprecated_alias('construct_nvme_bdev')
def bdev_nvme_attach_controller(client, name, trtype, traddr, adrfam=None, trsvcid=None,
                                priority=None, subnqn=None, hostnqn=None, hostaddr=None,
                                hostsvcid=None, prchk_reftag=None, prchk_guard=None):
    """Construct block device for each NVMe namespace in the attached controller.

    Args:
        name: bdev name prefix; "n" + namespace ID will be appended to create unique names
        trtype: transport type ("PCIe", "RDMA", "FC", "TCP")
        traddr: transport address (PCI BDF or IP address)
        adrfam: address family ("IPv4", "IPv6", "IB", or "FC")
        trsvcid: transport service ID (port number for IP-based addresses)
        priority: transport connection priority (Sock priority for TCP-based transports; optional)
        subnqn: subsystem NQN to connect to (optional)
        hostnqn: NQN to connect from (optional)
        hostaddr: host transport address (IP address for IP-based transports, NULL for PCIe or FC; optional)
        hostsvcid: host transport service ID (port number for IP-based transports, NULL for PCIe or FC; optional)
        prchk_reftag: Enable checking of PI reference tag for I/O processing (optional)
        prchk_guard: Enable checking of PI guard for I/O processing (optional)

    Returns:
        Names of created block devices.
    """
    params = {'name': name,
              'trtype': trtype,
              'traddr': traddr}

    if hostnqn:
        params['hostnqn'] = hostnqn

    if hostaddr:
        params['hostaddr'] = hostaddr

    if hostsvcid:
        params['hostsvcid'] = hostsvcid

    if adrfam:
        params['adrfam'] = adrfam

    if trsvcid:
        params['trsvcid'] = trsvcid

    if priority:
        params['priority'] = priority

    if subnqn:
        params['subnqn'] = subnqn

    if prchk_reftag:
        params['prchk_reftag'] = prchk_reftag

    if prchk_guard:
        params['prchk_guard'] = prchk_guard

    return client.call('bdev_nvme_attach_controller', params)


@deprecated_alias('delete_nvme_controller')
def bdev_nvme_detach_controller(client, name, trtype=None, traddr=None,
                                adrfam=None, trsvcid=None, subnqn=None):
    """Detach NVMe controller and delete any associated bdevs. Optionally,
       If all of the transport ID options are specified, only remove that
       transport path from the specified controller. If that is the only
       available path for the controller, this will also result in the
       controller being detached and the associated bdevs being deleted.

    Args:
        name: controller name
        trtype: transport type ("PCIe", "RDMA")
        traddr: transport address (PCI BDF or IP address)
        adrfam: address family ("IPv4", "IPv6", "IB", or "FC")
        trsvcid: transport service ID (port number for IP-based addresses)
        subnqn: subsystem NQN to connect to (optional)
    """

    params = {'name': name}

    if trtype:
        params['trtype'] = trtype

    if traddr:
        params['traddr'] = traddr

    if adrfam:
        params['adrfam'] = adrfam

    if trsvcid:
        params['trsvcid'] = trsvcid

    if subnqn:
        params['subnqn'] = subnqn

    return client.call('bdev_nvme_detach_controller', params)


def bdev_nvme_cuse_register(client, name):
    """Register CUSE devices on NVMe controller.

    Args:
        name: Name of the operating NVMe controller
    """
    params = {'name': name}

    return client.call('bdev_nvme_cuse_register', params)


def bdev_nvme_cuse_unregister(client, name):
    """Unregister CUSE devices on NVMe controller.

    Args:
        name: Name of the operating NVMe controller
    """
    params = {'name': name}

    return client.call('bdev_nvme_cuse_unregister', params)


def bdev_zone_block_create(client, name, base_bdev, zone_capacity, optimal_open_zones):
    """Creates a virtual zone device on top of existing non-zoned bdev.

    Args:
        name: Zone device name
        base_bdev: Base Nvme bdev name
        zone_capacity: Surfaced zone capacity in blocks
        optimal_open_zones: Number of zones required to reach optimal write speed (optional, default: 1)

    Returns:
        Name of created block device.
    """
    params = {'name': name,
              'base_bdev': base_bdev,
              'zone_capacity': zone_capacity,
              'optimal_open_zones': optimal_open_zones}

    return client.call('bdev_zone_block_create', params)


def bdev_zone_block_delete(client, name):
    """Remove block zone bdev from the system.

    Args:
        name: name of block zone bdev to delete
    """
    params = {'name': name}
    return client.call('bdev_zone_block_delete', params)


@deprecated_alias('construct_rbd_bdev')
def bdev_rbd_create(client, pool_name, rbd_name, block_size, name=None, user=None, config=None):
    """Create a Ceph RBD block device.

    Args:
        pool_name: Ceph RBD pool name
        rbd_name: Ceph RBD image name
        block_size: block size of RBD volume
        name: name of block device (optional)
        user: Ceph user name (optional)
        config: map of config keys to values (optional)

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
    if user is not None:
        params['user_id'] = user
    if config is not None:
        params['config'] = config

    return client.call('bdev_rbd_create', params)


@deprecated_alias('delete_rbd_bdev')
def bdev_rbd_delete(client, name):
    """Remove rbd bdev from the system.

    Args:
        name: name of rbd bdev to delete
    """
    params = {'name': name}
    return client.call('bdev_rbd_delete', params)


def bdev_rbd_resize(client, name, new_size):
    """Resize rbd bdev in the system.

    Args:
        name: name of rbd bdev to resize
        new_size: new bdev size of resize operation. The unit is MiB
    """
    params = {
            'name': name,
            'new_size': new_size,
            }
    return client.call('bdev_rbd_resize', params)


@deprecated_alias('construct_error_bdev')
def bdev_error_create(client, base_name):
    """Construct an error injection block device.

    Args:
        base_name: base bdev name
    """
    params = {'base_name': base_name}
    return client.call('bdev_error_create', params)


def bdev_delay_create(client, base_bdev_name, name, avg_read_latency, p99_read_latency, avg_write_latency, p99_write_latency):
    """Construct a delay block device.

    Args:
        base_bdev_name: name of the existing bdev
        name: name of block device
        avg_read_latency: complete 99% of read ops with this delay
        p99_read_latency: complete 1% of read ops with this delay
        avg_write_latency: complete 99% of write ops with this delay
        p99_write_latency: complete 1% of write ops with this delay

    Returns:
        Name of created block device.
    """
    params = {
        'base_bdev_name': base_bdev_name,
        'name': name,
        'avg_read_latency': avg_read_latency,
        'p99_read_latency': p99_read_latency,
        'avg_write_latency': avg_write_latency,
        'p99_write_latency': p99_write_latency,
    }
    return client.call('bdev_delay_create', params)


def bdev_delay_delete(client, name):
    """Remove delay bdev from the system.

    Args:
        name: name of delay bdev to delete
    """
    params = {'name': name}
    return client.call('bdev_delay_delete', params)


def bdev_delay_update_latency(client, delay_bdev_name, latency_type, latency_us):
    """Update the latency value for a delay block device

    Args:
        delay_bdev_name: name of the delay bdev
        latency_type: 'one of: avg_read, avg_write, p99_read, p99_write. No other values accepted.'
        latency_us: 'new latency value.'

    Returns:
        True if successful, or a specific error otherwise.
    """
    params = {
        'delay_bdev_name': delay_bdev_name,
        'latency_type': latency_type,
        'latency_us': latency_us,
    }
    return client.call('bdev_delay_update_latency', params)


@deprecated_alias('delete_error_bdev')
def bdev_error_delete(client, name):
    """Remove error bdev from the system.

    Args:
        bdev_name: name of error bdev to delete
    """
    params = {'name': name}
    return client.call('bdev_error_delete', params)


@deprecated_alias('construct_iscsi_bdev')
def bdev_iscsi_create(client, name, url, initiator_iqn):
    """Construct an iSCSI block device.

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
    return client.call('bdev_iscsi_create', params)


@deprecated_alias('delete_iscsi_bdev')
def bdev_iscsi_delete(client, name):
    """Remove iSCSI bdev from the system.

    Args:
        bdev_name: name of iSCSI bdev to delete
    """
    params = {'name': name}
    return client.call('bdev_iscsi_delete', params)


@deprecated_alias('construct_pmem_bdev')
def bdev_pmem_create(client, pmem_file, name):
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
    return client.call('bdev_pmem_create', params)


@deprecated_alias('delete_pmem_bdev')
def bdev_pmem_delete(client, name):
    """Remove pmem bdev from the system.

    Args:
        name: name of pmem bdev to delete
    """
    params = {'name': name}
    return client.call('bdev_pmem_delete', params)


@deprecated_alias('construct_passthru_bdev')
def bdev_passthru_create(client, base_bdev_name, name):
    """Construct a pass-through block device.

    Args:
        base_bdev_name: name of the existing bdev
        name: name of block device

    Returns:
        Name of created block device.
    """
    params = {
        'base_bdev_name': base_bdev_name,
        'name': name,
    }
    return client.call('bdev_passthru_create', params)


@deprecated_alias('delete_passthru_bdev')
def bdev_passthru_delete(client, name):
    """Remove pass through bdev from the system.

    Args:
        name: name of pass through bdev to delete
    """
    params = {'name': name}
    return client.call('bdev_passthru_delete', params)


def bdev_opal_create(client, nvme_ctrlr_name, nsid, locking_range_id, range_start, range_length, password):
    """Create opal virtual block devices from a base nvme bdev.

    Args:
        nvme_ctrlr_name: name of the nvme ctrlr
        nsid: namespace ID of nvme ctrlr
        locking_range_id: locking range ID corresponding to this virtual bdev
        range_start: start address of this locking range
        range_length: length of this locking range
        password: admin password of base nvme bdev

    Returns:
        Name of the new created block devices.
    """
    params = {
        'nvme_ctrlr_name': nvme_ctrlr_name,
        'nsid': nsid,
        'locking_range_id': locking_range_id,
        'range_start': range_start,
        'range_length': range_length,
        'password': password,
    }

    return client.call('bdev_opal_create', params)


def bdev_opal_get_info(client, bdev_name, password):
    """Get opal locking range info.

    Args:
        bdev_name: name of opal vbdev to get info
        password: admin password

    Returns:
        Locking range info.
    """
    params = {
        'bdev_name': bdev_name,
        'password': password,
    }

    return client.call('bdev_opal_get_info', params)


def bdev_opal_delete(client, bdev_name, password):
    """Delete opal virtual bdev from the system.

    Args:
        bdev_name: name of opal vbdev to delete
        password: admin password of base nvme bdev
    """
    params = {
        'bdev_name': bdev_name,
        'password': password,
    }

    return client.call('bdev_opal_delete', params)


def bdev_opal_new_user(client, bdev_name, admin_password, user_id, user_password):
    """Add a user to opal bdev who can set lock state for this bdev.

    Args:
        bdev_name: name of opal vbdev
        admin_password: admin password
        user_id: ID of the user who will be added to this opal bdev
        user_password: password set for this user
    """
    params = {
        'bdev_name': bdev_name,
        'admin_password': admin_password,
        'user_id': user_id,
        'user_password': user_password,
    }

    return client.call('bdev_opal_new_user', params)


def bdev_opal_set_lock_state(client, bdev_name, user_id, password, lock_state):
    """set lock state for an opal bdev.

    Args:
        bdev_name: name of opal vbdev
        user_id: ID of the user who will set lock state
        password: password of the user
        lock_state: lock state to set
    """
    params = {
        'bdev_name': bdev_name,
        'user_id': user_id,
        'password': password,
        'lock_state': lock_state,
    }

    return client.call('bdev_opal_set_lock_state', params)


@deprecated_alias('construct_split_vbdev')
def bdev_split_create(client, base_bdev, split_count, split_size_mb=None):
    """Create split block devices from a base bdev.

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

    return client.call('bdev_split_create', params)


@deprecated_alias('destruct_split_vbdev')
def bdev_split_delete(client, base_bdev):
    """Delete split block devices.

    Args:
        base_bdev: name of previously split bdev
    """
    params = {
        'base_bdev': base_bdev,
    }

    return client.call('bdev_split_delete', params)


@deprecated_alias('construct_ftl_bdev')
def bdev_ftl_create(client, name, base_bdev, **kwargs):
    """Construct FTL bdev

    Args:
        name: name of the bdev
        base_bdev: name of the base bdev
        kwargs: optional parameters
    """
    params = {'name': name,
              'base_bdev': base_bdev}
    for key, value in kwargs.items():
        if value is not None:
            params[key] = value

    return client.call('bdev_ftl_create', params)


@deprecated_alias('delete_ftl_bdev')
def bdev_ftl_delete(client, name):
    """Delete FTL bdev

    Args:
        name: name of the bdev
    """
    params = {'name': name}

    return client.call('bdev_ftl_delete', params)


def bdev_ocssd_create(client, ctrlr_name, bdev_name, nsid=None, range=None):
    """Creates Open Channel zoned bdev on specified Open Channel controller

    Args:
        ctrlr_name: name of the OC NVMe controller
        bdev_name: name of the bdev to create
        nsid: namespace ID
        range: parallel unit range
    """
    params = {'ctrlr_name': ctrlr_name,
              'bdev_name': bdev_name}

    if nsid is not None:
        params['nsid'] = nsid

    if range is not None:
        params['range'] = range

    return client.call('bdev_ocssd_create', params)


def bdev_ocssd_delete(client, name):
    """Deletes Open Channel bdev

    Args:
        name: name of the bdev
    """
    params = {'name': name}

    return client.call('bdev_ocssd_delete', params)


@deprecated_alias('get_bdevs')
def bdev_get_bdevs(client, name=None):
    """Get information about block devices.

    Args:
        name: bdev name to query (optional; if omitted, query all bdevs)

    Returns:
        List of bdev information objects.
    """
    params = {}
    if name:
        params['name'] = name
    return client.call('bdev_get_bdevs', params)


@deprecated_alias('get_bdevs_iostat')
def bdev_get_iostat(client, name=None):
    """Get I/O statistics for block devices.

    Args:
        name: bdev name to query (optional; if omitted, query all bdevs)

    Returns:
        I/O statistics for the requested block devices.
    """
    params = {}
    if name:
        params['name'] = name
    return client.call('bdev_get_iostat', params)


@deprecated_alias('enable_bdev_histogram')
def bdev_enable_histogram(client, name, enable):
    """Control whether histogram is enabled for specified bdev.

    Args:
        bdev_name: name of bdev
    """
    params = {'name': name, "enable": enable}
    return client.call('bdev_enable_histogram', params)


@deprecated_alias('get_bdev_histogram')
def bdev_get_histogram(client, name):
    """Get histogram for specified bdev.

    Args:
        bdev_name: name of bdev
    """
    params = {'name': name}
    return client.call('bdev_get_histogram', params)


@deprecated_alias('bdev_inject_error')
def bdev_error_inject_error(client, name, io_type, error_type, num=1):
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

    return client.call('bdev_error_inject_error', params)


@deprecated_alias('set_bdev_qd_sampling_period')
def bdev_set_qd_sampling_period(client, name, period):
    """Enable queue depth tracking on a specified bdev.

    Args:
        name: name of a bdev on which to track queue depth.
        period: period (in microseconds) at which to update the queue depth reading. If set to 0, polling will be disabled.
    """

    params = {}
    params['name'] = name
    params['period'] = period
    return client.call('bdev_set_qd_sampling_period', params)


@deprecated_alias('set_bdev_qos_limit')
def bdev_set_qos_limit(
        client,
        name,
        rw_ios_per_sec=None,
        rw_mbytes_per_sec=None,
        r_mbytes_per_sec=None,
        w_mbytes_per_sec=None):
    """Set QoS rate limit on a block device.

    Args:
        name: name of block device
        rw_ios_per_sec: R/W IOs per second limit (>=1000, example: 20000). 0 means unlimited.
        rw_mbytes_per_sec: R/W megabytes per second limit (>=10, example: 100). 0 means unlimited.
        r_mbytes_per_sec: Read megabytes per second limit (>=10, example: 100). 0 means unlimited.
        w_mbytes_per_sec: Write megabytes per second limit (>=10, example: 100). 0 means unlimited.
    """
    params = {}
    params['name'] = name
    if rw_ios_per_sec is not None:
        params['rw_ios_per_sec'] = rw_ios_per_sec
    if rw_mbytes_per_sec is not None:
        params['rw_mbytes_per_sec'] = rw_mbytes_per_sec
    if r_mbytes_per_sec is not None:
        params['r_mbytes_per_sec'] = r_mbytes_per_sec
    if w_mbytes_per_sec is not None:
        params['w_mbytes_per_sec'] = w_mbytes_per_sec
    return client.call('bdev_set_qos_limit', params)


@deprecated_alias('apply_firmware')
def bdev_nvme_apply_firmware(client, bdev_name, filename):
    """Download and commit firmware to NVMe device.

    Args:
        bdev_name: name of NVMe block device
        filename: filename of the firmware to download
    """
    params = {
        'filename': filename,
        'bdev_name': bdev_name,
    }
    return client.call('bdev_nvme_apply_firmware', params)
