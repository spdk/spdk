#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2017 Intel Corporation.
#  All rights reserved.
#  Copyright (c) 2022 Dell Inc, or its subsidiaries.
#  Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.


def bdev_set_options(client, bdev_io_pool_size=None, bdev_io_cache_size=None,
                     bdev_auto_examine=None, iobuf_small_cache_size=None,
                     iobuf_large_cache_size=None):
    """Set parameters for the bdev subsystem.
    Args:
        bdev_io_pool_size: number of bdev_io structures in shared buffer pool (optional)
        bdev_io_cache_size: maximum number of bdev_io structures cached per thread (optional)
        bdev_auto_examine: if set to false, the bdev layer will not examine every disks automatically (optional)
        iobuf_small_cache_size: size of the small iobuf per thread cache
        iobuf_large_cache_size: size of the large iobuf per thread cache
    """
    params = dict()
    if bdev_io_pool_size is not None:
        params['bdev_io_pool_size'] = bdev_io_pool_size
    if bdev_io_cache_size is not None:
        params['bdev_io_cache_size'] = bdev_io_cache_size
    if bdev_auto_examine is not None:
        params['bdev_auto_examine'] = bdev_auto_examine
    if iobuf_small_cache_size is not None:
        params['iobuf_small_cache_size'] = iobuf_small_cache_size
    if iobuf_large_cache_size is not None:
        params['iobuf_large_cache_size'] = iobuf_large_cache_size
    return client.call('bdev_set_options', params)


def bdev_examine(client, name):
    """Examine a bdev manually. If the bdev does not exist yet when this RPC is called,
    it will be examined when it is created
    Args:
        name: name of the bdev
    """
    params = dict()
    params['name'] = name
    return client.call('bdev_examine', params)


def bdev_wait_for_examine(client):
    """Report when all bdevs have been examined
    """
    return client.call('bdev_wait_for_examine')


def bdev_compress_create(client, base_bdev_name, pm_path, lb_size=None, comp_algo=None, comp_level=None):
    """Construct a compress virtual block device.
    Args:
        base_bdev_name: name of the underlying base bdev
        pm_path: path to persistent memory
        lb_size: logical block size for the compressed vol in bytes.  Must be 4K or 512.
        comp_algo: compression algorithm for the compressed vol. Default is deflate.
        comp_level: compression algorithm level for the compressed vol. Default is 1.
    Returns:
        Name of created virtual block device.
    """
    params = dict()
    params['base_bdev_name'] = base_bdev_name
    params['pm_path'] = pm_path
    if lb_size is not None:
        params['lb_size'] = lb_size
    if comp_algo is not None:
        params['comp_algo'] = comp_algo
    if comp_level is not None:
        params['comp_level'] = comp_level
    return client.call('bdev_compress_create', params)


def bdev_compress_delete(client, name):
    """Delete compress virtual block device.
    Args:
        name: name of compress vbdev to delete
    """
    params = dict()
    params['name'] = name
    return client.call('bdev_compress_delete', params)


def bdev_compress_get_orphans(client, name=None):
    """Get a list of comp bdevs that do not have a pmem file (aka orphaned).
    Args:
        name: comp bdev name to query (optional; if omitted, query all comp bdevs)
    Returns:
        List of comp bdev names.
    """
    params = dict()
    if name is not None:
        params['name'] = name
    return client.call('bdev_compress_get_orphans', params)


def bdev_crypto_create(client, base_bdev_name, name, crypto_pmd=None, key=None, cipher=None, key2=None, key_name=None):
    """Construct a crypto virtual block device.
    Args:
        base_bdev_name: name of the underlying base bdev
        name: name for the crypto vbdev
        crypto_pmd: name of the DPDK crypto driver to use
        key: key
        cipher: crypto algorithm to use
        key2: Optional second part of the key
        key_name: The key name to use in crypto operations
    Returns:
        Name of created virtual block device.
    """
    params = dict()
    params['base_bdev_name'] = base_bdev_name
    params['name'] = name
    if crypto_pmd is not None:
        params['crypto_pmd'] = crypto_pmd
    if key is not None:
        params['key'] = key
    if cipher is not None:
        params['cipher'] = cipher
    if key2 is not None:
        params['key2'] = key2
    if key_name is not None:
        params['key_name'] = key_name
    return client.call('bdev_crypto_create', params)


def bdev_crypto_delete(client, name):
    """Delete crypto virtual block device.
    Args:
        name: name of crypto vbdev to delete
    """
    params = dict()
    params['name'] = name
    return client.call('bdev_crypto_delete', params)


def bdev_ocf_create(client, name, mode, cache_bdev_name, core_bdev_name, cache_line_size=None):
    """Add an OCF block device
    Args:
        name: name of constructed OCF bdev
        mode: OCF cache mode: {'wb', 'wt', 'pt', 'wa', 'wi', 'wo'}
        cache_bdev_name: name of underlying cache bdev
        core_bdev_name: name of underlying core bdev
        cache_line_size: OCF cache line size. The unit is KiB: {4, 8, 16, 32, 64}
    Returns:
        Name of created block device
    """
    params = dict()
    params['name'] = name
    params['mode'] = mode
    params['cache_bdev_name'] = cache_bdev_name
    params['core_bdev_name'] = core_bdev_name
    if cache_line_size is not None:
        params['cache_line_size'] = cache_line_size
    return client.call('bdev_ocf_create', params)


def bdev_ocf_delete(client, name):
    """Delete an OCF device
    Args:
        name: name of OCF bdev
    """
    params = dict()
    params['name'] = name
    return client.call('bdev_ocf_delete', params)


def bdev_ocf_get_stats(client, name):
    """Get statistics of chosen OCF block device
    Args:
        name: name of OCF bdev
    Returns:
        Statistics as json object
    """
    params = dict()
    params['name'] = name
    return client.call('bdev_ocf_get_stats', params)


def bdev_ocf_reset_stats(client, name):
    """Reset statistics of chosen OCF block device
    Args:
        name: name of OCF bdev
    Returns:
        None
    """
    params = dict()
    params['name'] = name
    return client.call('bdev_ocf_reset_stats', params)


def bdev_ocf_get_bdevs(client, name=None):
    """Get list of OCF devices including unregistered ones
    Args:
        name: name of OCF vbdev or name of cache device or name of core device (optional)
    Returns:
        Array of OCF devices with their current status
    """
    params = dict()
    if name is not None:
        params['name'] = name
    return client.call('bdev_ocf_get_bdevs', params)


def bdev_ocf_set_cache_mode(client, name, mode):
    """Set cache mode of OCF block device
    Args:
        name: name of OCF bdev
        mode: OCF cache mode: {'wb', 'wt', 'pt', 'wa', 'wi', 'wo'}
    Returns:
        New cache mode name
    """
    params = dict()
    params['name'] = name
    params['mode'] = mode
    return client.call('bdev_ocf_set_cache_mode', params)


def bdev_ocf_set_seqcutoff(client, name, policy, threshold=None, promotion_count=None):
    """Set sequential cutoff parameters on all cores for the given OCF cache device
    Args:
        name: Name of OCF cache bdev
        policy: Sequential cutoff policy
        threshold: Activation threshold [KiB] (optional)
        promotion_count: Promotion request count (optional)
    """
    params = dict()
    params['name'] = name
    params['policy'] = policy
    if threshold is not None:
        params['threshold'] = threshold
    if promotion_count is not None:
        params['promotion_count'] = promotion_count
    return client.call('bdev_ocf_set_seqcutoff', params)


def bdev_ocf_flush_start(client, name):
    """Start flushing OCF cache device
    Args:
        name: name of OCF bdev
    """
    params = dict()
    params['name'] = name
    return client.call('bdev_ocf_flush_start', params)


def bdev_ocf_flush_status(client, name):
    """Get flush status of OCF cache device
    Args:
        name: name of OCF bdev
    Returns:
        Flush status
    """
    params = dict()
    params['name'] = name
    return client.call('bdev_ocf_flush_status', params)


def bdev_malloc_create(client, num_blocks, block_size, physical_block_size=None, name=None, uuid=None, optimal_io_boundary=None,
                       md_size=None, md_interleave=None, dif_type=None, dif_is_head_of_md=None, dif_pi_format=None):
    """Construct a malloc block device.
    Args:
        num_blocks: size of block device in blocks
        block_size: Data block size of device; must be a power of 2 and at least 512
        physical_block_size: Physical block size of device; must be a power of 2 and at least 512 (optional)
        name: name of block device (optional)
        uuid: UUID of block device (optional)
        optimal_io_boundary: Split on optimal IO boundary, in number of blocks, default 0 (disabled, optional)
        md_size: metadata size of device (0, 8, 16, 32, 64, or 128), default 0 (optional)
        md_interleave: metadata location, interleaved if set, and separated if omitted (optional)
        dif_type: protection information type (optional)
        dif_is_head_of_md: protection information is in the first 8 bytes of metadata (optional)
        dif_pi_format: protection information format (optional)
    Returns:
        Name of created block device.
    """
    params = dict()
    params['num_blocks'] = num_blocks
    params['block_size'] = block_size
    if physical_block_size is not None:
        params['physical_block_size'] = physical_block_size
    if name is not None:
        params['name'] = name
    if uuid is not None:
        params['uuid'] = uuid
    if optimal_io_boundary is not None:
        params['optimal_io_boundary'] = optimal_io_boundary
    if md_size is not None:
        params['md_size'] = md_size
    if md_interleave is not None:
        params['md_interleave'] = md_interleave
    if dif_type is not None:
        params['dif_type'] = dif_type
    if dif_is_head_of_md is not None:
        params['dif_is_head_of_md'] = dif_is_head_of_md
    if dif_pi_format is not None:
        params['dif_pi_format'] = dif_pi_format
    return client.call('bdev_malloc_create', params)


def bdev_malloc_delete(client, name):
    """Delete malloc block device.
    Args:
        bdev_name: name of malloc bdev to delete
    """
    params = dict()
    params['name'] = name
    return client.call('bdev_malloc_delete', params)


def bdev_null_create(client, num_blocks, block_size, name, physical_block_size=None, uuid=None, md_size=None,
                     dif_type=None, dif_is_head_of_md=None, dif_pi_format=None):
    """Construct a null block device.
    Args:
        num_blocks: size of block device in blocks
        block_size: block size of device; data part size must be a power of 2 and at least 512
        name: name of block device
        physical_block_size: physical block size of the device; data part size must be a power of 2 and at least 512 (optional)
        uuid: UUID of block device (optional)
        md_size: metadata size of device (optional)
        dif_type: protection information type (optional)
        dif_is_head_of_md: protection information is in the first 8 bytes of metadata (optional)
        dif_pi_format: protection information format (optional)
    Returns:
        Name of created block device.
    """
    params = dict()
    params['num_blocks'] = num_blocks
    params['block_size'] = block_size
    params['name'] = name
    if physical_block_size is not None:
        params['physical_block_size'] = physical_block_size
    if uuid is not None:
        params['uuid'] = uuid
    if md_size is not None:
        params['md_size'] = md_size
    if dif_type is not None:
        params['dif_type'] = dif_type
    if dif_is_head_of_md is not None:
        params['dif_is_head_of_md'] = dif_is_head_of_md
    if dif_pi_format is not None:
        params['dif_pi_format'] = dif_pi_format
    return client.call('bdev_null_create', params)


def bdev_null_delete(client, name):
    """Remove null bdev from the system.
    Args:
        name: name of null bdev to delete
    """
    params = dict()
    params['name'] = name
    return client.call('bdev_null_delete', params)


def bdev_null_resize(client, name, new_size):
    """Resize null bdev in the system.
    Args:
        name: name of null bdev to resize
        new_size: new bdev size of resize operation. The unit is MiB
    """
    params = dict()
    params['name'] = name
    params['new_size'] = new_size
    return client.call('bdev_null_resize', params)


def bdev_raid_set_options(client, process_window_size_kb=None, process_max_bandwidth_mb_sec=None):
    """Set options for bdev raid.
    Args:
        process_window_size_kb: Background process (e.g. rebuild) window size in KiB
        process_max_bandwidth_mb_sec: Background process (e.g. rebuild) maximum bandwidth in MiB/Sec
    """
    params = dict()
    if process_window_size_kb is not None:
        params['process_window_size_kb'] = process_window_size_kb

    if process_max_bandwidth_mb_sec is not None:
        params['process_max_bandwidth_mb_sec'] = process_max_bandwidth_mb_sec

    return client.call('bdev_raid_set_options', params)


def bdev_raid_get_bdevs(client, category):
    """Get list of raid bdevs based on category
    Args:
        category: any one of all or online or configuring or offline
    Returns:
        List of raid bdev details
    """
    params = dict()
    params['category'] = category
    return client.call('bdev_raid_get_bdevs', params)


def bdev_raid_create(client, name, raid_level, base_bdevs, strip_size_kb=None, uuid=None, superblock=None):
    """Create raid bdev. Either strip size arg will work but one is required.
    Args:
        name: user defined raid bdev name
        raid_level: raid level of raid bdev, supported values 0
        base_bdevs: Space separated names of Nvme bdevs in double quotes, like "Nvme0n1 Nvme1n1 Nvme2n1"
        strip_size_kb: strip size of raid bdev in KB, supported values like 8, 16, 32, 64, 128, 256, etc
        uuid: UUID for this raid bdev (optional)
        superblock: information about raid bdev will be stored in superblock on each base bdev,
                    disabled by default due to backward compatibility
    Returns:
        None
    """
    params = dict()
    params['name'] = name
    params['raid_level'] = raid_level
    params['base_bdevs'] = base_bdevs
    if strip_size_kb is not None:
        params['strip_size_kb'] = strip_size_kb
    if uuid is not None:
        params['uuid'] = uuid
    if superblock is not None:
        params['superblock'] = superblock
    return client.call('bdev_raid_create', params)


def bdev_raid_delete(client, name):
    """Delete raid bdev
    Args:
        name: raid bdev name
    Returns:
        None
    """
    params = dict()
    params['name'] = name
    return client.call('bdev_raid_delete', params)


def bdev_raid_add_base_bdev(client, base_bdev, raid_bdev):
    """Add base bdev to existing raid bdev
    Args:
        base_bdev: base bdev name
        raid_bdev: raid bdev name
    Returns:
        None
    """
    params = dict()
    params['base_bdev'] = base_bdev
    params['raid_bdev'] = raid_bdev
    return client.call('bdev_raid_add_base_bdev', params)


def bdev_raid_remove_base_bdev(client, name):
    """Remove base bdev from existing raid bdev
    Args:
        name: base bdev name
    Returns:
        None
    """
    params = dict()
    params['name'] = name
    return client.call('bdev_raid_remove_base_bdev', params)


def bdev_aio_create(client, filename, name, block_size=None, readonly=None, fallocate=None, uuid=None):
    """Construct a Linux AIO block device.
    Args:
        filename: path to device or file (ex: /dev/sda)
        name: name of block device
        block_size: block size of device (optional; autodetected if omitted)
        readonly: set aio bdev as read-only
        fallocate: enable fallocate for UNMAP/WRITEZEROS support (note that fallocate syscall would block reactor)
        uuid: UUID of the bdev (optional)
    Returns:
        Name of created block device.
    """
    params = dict()
    params['filename'] = filename
    params['name'] = name
    if block_size is not None:
        params['block_size'] = block_size
    if readonly is not None:
        params['readonly'] = readonly
    if fallocate is not None:
        params['fallocate'] = fallocate
    if uuid is not None:
        params['uuid'] = uuid
    return client.call('bdev_aio_create', params)


def bdev_aio_rescan(client, name):
    """Rescan a Linux AIO block device.
    Args:
        name: name of aio bdev to rescan
    """
    params = dict()
    params['name'] = name
    return client.call('bdev_aio_rescan', params)


def bdev_aio_delete(client, name):
    """Remove aio bdev from the system.
    Args:
        name: name of aio bdev to delete
    """
    params = dict()
    params['name'] = name
    return client.call('bdev_aio_delete', params)


def bdev_uring_create(client, filename, name, block_size=None, uuid=None):
    """Create a bdev with Linux io_uring backend.
    Args:
        filename: path to device or file (ex: /dev/nvme0n1)
        name: name of bdev
        block_size: block size of device (optional; autodetected if omitted)
        uuid: UUID of block device (optional)
    Returns:
        Name of created bdev.
    """
    params = dict()
    params['filename'] = filename
    params['name'] = name
    if block_size is not None:
        params['block_size'] = block_size
    if uuid is not None:
        params['uuid'] = uuid
    return client.call('bdev_uring_create', params)


def bdev_uring_rescan(client, name):
    """Rescan a Linux URING block device.
    Args:
        name: name of uring bdev to rescan
    """
    params = dict()
    params['name'] = name
    return client.call('bdev_uring_rescan', params)


def bdev_uring_delete(client, name):
    """Delete a uring bdev.
    Args:
        name: name of uring bdev to delete
    """
    params = dict()
    params['name'] = name
    return client.call('bdev_uring_delete', params)


def bdev_xnvme_create(client, filename, name, io_mechanism, conserve_cpu=None):
    """Create a bdev with xNVMe backend.
    Args:
        filename: path to device or file (ex: /dev/nvme0n1)
        name: name of xNVMe bdev to create
        io_mechanism: I/O mechanism to use (ex: io_uring, io_uring_cmd, etc.)
        conserve_cpu: Whether or not to conserve CPU when polling (default: False)
    Returns:
        Name of created bdev.
    """
    params = dict()
    params['filename'] = filename
    params['name'] = name
    params['io_mechanism'] = io_mechanism
    if conserve_cpu is not None:
        params['conserve_cpu'] = conserve_cpu
    return client.call('bdev_xnvme_create', params)


def bdev_xnvme_delete(client, name):
    """Delete a xNVMe bdev.
    Args:
        name: name of xNVMe bdev to delete
    """
    params = dict()
    params['name'] = name
    return client.call('bdev_xnvme_delete', params)


def bdev_nvme_set_options(client, action_on_timeout=None, timeout_us=None, timeout_admin_us=None,
                          keep_alive_timeout_ms=None, arbitration_burst=None,
                          low_priority_weight=None, medium_priority_weight=None, high_priority_weight=None,
                          nvme_adminq_poll_period_us=None, nvme_ioq_poll_period_us=None, io_queue_requests=None,
                          delay_cmd_submit=None, transport_retry_count=None, bdev_retry_count=None,
                          transport_ack_timeout=None, ctrlr_loss_timeout_sec=None, reconnect_delay_sec=None,
                          fast_io_fail_timeout_sec=None, disable_auto_failback=None, generate_uuids=None,
                          transport_tos=None, nvme_error_stat=None, rdma_srq_size=None, io_path_stat=None,
                          allow_accel_sequence=None, rdma_max_cq_size=None, rdma_cm_event_timeout_ms=None,
                          dhchap_digests=None, dhchap_dhgroups=None):
    """Set options for the bdev nvme. This is startup command.
    Args:
        action_on_timeout:  action to take on command time out. Valid values are: none, reset, abort (optional)
        timeout_us: Timeout for each command, in microseconds. If 0, don't track timeouts (optional)
        timeout_admin_us: Timeout for each admin command, in microseconds. If 0, treat same as io timeouts (optional)
        keep_alive_timeout_ms: Keep alive timeout period in millisecond, default is 10s (optional)
        arbitration_burst: The value is expressed as a power of two (optional)
        low_priority_weight: The number of commands that may be executed from the low priority queue at one time (optional)
        medium_priority_weight: The number of commands that may be executed from the medium priority queue at one time (optional)
        high_priority_weight: The number of commands that may be executed from the high priority queue at one time (optional)
        nvme_adminq_poll_period_us: How often the admin queue is polled for asynchronous events in microseconds (optional)
        nvme_ioq_poll_period_us: How often to poll I/O queues for completions in microseconds (optional)
        io_queue_requests: The number of requests allocated for each NVMe I/O queue. Default: 512 (optional)
        delay_cmd_submit: Enable delayed NVMe command submission to allow batching of multiple commands (optional)
        transport_retry_count: The number of attempts per I/O in the transport layer when an I/O fails (optional)
        bdev_retry_count: The number of attempts per I/O in the bdev layer when an I/O fails. -1 means infinite retries. (optional)
        transport_ack_timeout: Time to wait ack until packet retransmission for RDMA or until closes connection for TCP.
        Range 0-31 where 0 is driver-specific default value (optional)
        ctrlr_loss_timeout_sec: Time to wait until ctrlr is reconnected before deleting ctrlr.
        -1 means infinite reconnect retries. 0 means no reconnect retry.
        If reconnect_delay_sec is zero, ctrlr_loss_timeout_sec has to be zero.
        If reconnect_delay_sec is non-zero, ctrlr_loss_timeout_sec has to be -1 or not less than reconnect_delay_sec.
        This can be overridden by bdev_nvme_attach_controller. (optional)
        reconnect_delay_sec: Time to delay a reconnect retry.
        If ctrlr_loss_timeout_sec is zero, reconnect_delay_sec has to be zero.
        If ctrlr_loss_timeout_sec is -1, reconnect_delay_sec has to be non-zero.
        If ctrlr_loss_timeout_sec is not -1 or zero, reconnect_sec has to be non-zero and less than ctrlr_loss_timeout_sec.
        This can be overridden by bdev_nvme_attach_controller. (optional)
        fast_io_fail_timeout_sec: Time to wait until ctrlr is reconnected before failing I/O to ctrlr.
        0 means no such timeout.
        If fast_io_fail_timeout_sec is not zero, it has to be not less than reconnect_delay_sec and less than
        ctrlr_loss_timeout_sec if ctrlr_loss_timeout_sec is not -1.
        This can be overridden by bdev_nvme_attach_controller. (optional)
        disable_auto_failback: Disable automatic failback. bdev_nvme_set_preferred_path can be used to do manual failback.
        By default, immediately failback to the preferred I/O path if it is restored. (optional)
        generate_uuids: Enable generation of unique identifiers for NVMe bdevs only if they do not provide UUID themselves.
        These strings are based on device serial number and namespace ID and will always be the same for that device.
        transport_tos: IPv4 Type of Service value. Only applicable for RDMA transports.
        The default is 0 which means no TOS is applied. (optional)
        nvme_error_stat: Enable collecting NVMe error counts. (optional)
        rdma_srq_size: Set the size of a shared rdma receive queue. Default: 0 (disabled) (optional)
        io_path_stat: Enable collection I/O path stat of each io path. (optional)
        allow_accel_sequence: Allow NVMe bdevs to advertise support for accel sequences if the
        controller also supports them. (optional)
        rdma_max_cq_size: The maximum size of a rdma completion queue. Default: 0 (unlimited) (optional)
        rdma_cm_event_timeout_ms: Time to wait for RDMA CM event. Only applicable for RDMA transports.
        dhchap_digests: List of allowed DH-HMAC-CHAP digests. (optional)
        dhchap_dhgroups: List of allowed DH-HMAC-CHAP DH groups. (optional)
    """
    params = dict()
    if action_on_timeout is not None:
        params['action_on_timeout'] = action_on_timeout
    if timeout_us is not None:
        params['timeout_us'] = timeout_us
    if timeout_admin_us is not None:
        params['timeout_admin_us'] = timeout_admin_us
    if keep_alive_timeout_ms is not None:
        params['keep_alive_timeout_ms'] = keep_alive_timeout_ms
    if arbitration_burst is not None:
        params['arbitration_burst'] = arbitration_burst
    if low_priority_weight is not None:
        params['low_priority_weight'] = low_priority_weight
    if medium_priority_weight is not None:
        params['medium_priority_weight'] = medium_priority_weight
    if high_priority_weight is not None:
        params['high_priority_weight'] = high_priority_weight
    if nvme_adminq_poll_period_us is not None:
        params['nvme_adminq_poll_period_us'] = nvme_adminq_poll_period_us
    if nvme_ioq_poll_period_us is not None:
        params['nvme_ioq_poll_period_us'] = nvme_ioq_poll_period_us
    if io_queue_requests is not None:
        params['io_queue_requests'] = io_queue_requests
    if delay_cmd_submit is not None:
        params['delay_cmd_submit'] = delay_cmd_submit
    if transport_retry_count is not None:
        params['transport_retry_count'] = transport_retry_count
    if bdev_retry_count is not None:
        params['bdev_retry_count'] = bdev_retry_count
    if transport_ack_timeout is not None:
        params['transport_ack_timeout'] = transport_ack_timeout
    if ctrlr_loss_timeout_sec is not None:
        params['ctrlr_loss_timeout_sec'] = ctrlr_loss_timeout_sec
    if reconnect_delay_sec is not None:
        params['reconnect_delay_sec'] = reconnect_delay_sec
    if fast_io_fail_timeout_sec is not None:
        params['fast_io_fail_timeout_sec'] = fast_io_fail_timeout_sec
    if disable_auto_failback is not None:
        params['disable_auto_failback'] = disable_auto_failback
    if generate_uuids is not None:
        params['generate_uuids'] = generate_uuids
    if transport_tos is not None:
        params['transport_tos'] = transport_tos
    if nvme_error_stat is not None:
        params['nvme_error_stat'] = nvme_error_stat
    if rdma_srq_size is not None:
        params['rdma_srq_size'] = rdma_srq_size
    if io_path_stat is not None:
        params['io_path_stat'] = io_path_stat
    if allow_accel_sequence is not None:
        params['allow_accel_sequence'] = allow_accel_sequence
    if rdma_max_cq_size is not None:
        params['rdma_max_cq_size'] = rdma_max_cq_size
    if rdma_cm_event_timeout_ms is not None:
        params['rdma_cm_event_timeout_ms'] = rdma_cm_event_timeout_ms
    if dhchap_digests is not None:
        params['dhchap_digests'] = dhchap_digests
    if dhchap_dhgroups is not None:
        params['dhchap_dhgroups'] = dhchap_dhgroups
    return client.call('bdev_nvme_set_options', params)


def bdev_nvme_set_hotplug(client, enable, period_us=None):
    """Set options for the bdev nvme. This is startup command.
    Args:
       enable: True to enable hotplug, False to disable.
       period_us: how often the hotplug is processed for insert and remove events. Set 0 to reset to default. (optional)
    """
    params = dict()
    params['enable'] = enable
    if period_us is not None:
        params['period_us'] = period_us
    return client.call('bdev_nvme_set_hotplug', params)


def bdev_nvme_attach_controller(client, name, trtype, traddr, adrfam=None, trsvcid=None,
                                priority=None, subnqn=None, hostnqn=None, hostaddr=None,
                                hostsvcid=None, prchk_reftag=None, prchk_guard=None,
                                hdgst=None, ddgst=None, fabrics_connect_timeout_us=None,
                                multipath=None, num_io_queues=None, ctrlr_loss_timeout_sec=None,
                                reconnect_delay_sec=None, fast_io_fail_timeout_sec=None,
                                psk=None, max_bdevs=None, dhchap_key=None, dhchap_ctrlr_key=None,
                                allow_unrecognized_csi=None):
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
        hdgst: Enable TCP header digest (optional)
        ddgst: Enable TCP data digest (optional)
        fabrics_connect_timeout_us: Fabrics connect timeout in us (optional)
        multipath: The behavior when multiple paths are created ("disable", "failover", or "multipath"; failover if not specified)
        num_io_queues: The number of IO queues to request during initialization. (optional)
        ctrlr_loss_timeout_sec: Time to wait until ctrlr is reconnected before deleting ctrlr.
        -1 means infinite reconnect retries. 0 means no reconnect retry.
        If reconnect_delay_sec is zero, ctrlr_loss_timeout_sec has to be zero.
        If reconnect_delay_sec is non-zero, ctrlr_loss_timeout_sec has to be -1 or not less than reconnect_delay_sec.
        (optional)
        reconnect_delay_sec: Time to delay a reconnect retry.
        If ctrlr_loss_timeout_sec is zero, reconnect_delay_sec has to be zero.
        If ctrlr_loss_timeout_sec is -1, reconnect_delay_sec has to be non-zero.
        If ctrlr_loss_timeout_sec is not -1 or zero, reconnect_sec has to be non-zero and less than ctrlr_loss_timeout_sec.
        (optional)
        fast_io_fail_timeout_sec: Time to wait until ctrlr is reconnected before failing I/O to ctrlr.
        0 means no such timeout.
        If fast_io_fail_timeout_sec is not zero, it has to be not less than reconnect_delay_sec and less than
        ctrlr_loss_timeout_sec if ctrlr_loss_timeout_sec is not -1. (optional)
        psk: Set PSK file path and enable TCP SSL socket implementation (optional)
        max_bdevs: Size of the name array for newly created bdevs. Default is 128. (optional)
        dhchap_key: DH-HMAC-CHAP key name.
        dhchap_ctrlr_key: DH-HMAC-CHAP controller key name.
        allow_unrecognized_csi: Allow attaching namespaces with unrecognized command set identifiers. These will only support NVMe
        passthrough.
    Returns:
        Names of created block devices.
    """
    params = dict()
    params['name'] = name
    params['trtype'] = trtype
    params['traddr'] = traddr
    if adrfam is not None:
        params['adrfam'] = adrfam
    if trsvcid is not None:
        params['trsvcid'] = trsvcid
    if priority is not None:
        params['priority'] = priority
    if subnqn is not None:
        params['subnqn'] = subnqn
    if hostnqn is not None:
        params['hostnqn'] = hostnqn
    if hostaddr is not None:
        params['hostaddr'] = hostaddr
    if hostsvcid is not None:
        params['hostsvcid'] = hostsvcid
    if prchk_reftag is not None:
        params['prchk_reftag'] = prchk_reftag
    if prchk_guard is not None:
        params['prchk_guard'] = prchk_guard
    if hdgst is not None:
        params['hdgst'] = hdgst
    if ddgst is not None:
        params['ddgst'] = ddgst
    if fabrics_connect_timeout_us is not None:
        params['fabrics_connect_timeout_us'] = fabrics_connect_timeout_us
    if multipath is not None:
        params['multipath'] = multipath
    if num_io_queues is not None:
        params['num_io_queues'] = num_io_queues
    if ctrlr_loss_timeout_sec is not None:
        params['ctrlr_loss_timeout_sec'] = ctrlr_loss_timeout_sec
    if reconnect_delay_sec is not None:
        params['reconnect_delay_sec'] = reconnect_delay_sec
    if fast_io_fail_timeout_sec is not None:
        params['fast_io_fail_timeout_sec'] = fast_io_fail_timeout_sec
    if psk is not None:
        params['psk'] = psk
    if max_bdevs is not None:
        params['max_bdevs'] = max_bdevs
    if dhchap_key is not None:
        params['dhchap_key'] = dhchap_key
    if dhchap_ctrlr_key is not None:
        params['dhchap_ctrlr_key'] = dhchap_ctrlr_key
    if allow_unrecognized_csi is not None:
        params['allow_unrecognized_csi'] = allow_unrecognized_csi
    return client.call('bdev_nvme_attach_controller', params)


def bdev_nvme_detach_controller(client, name, trtype=None, traddr=None,
                                adrfam=None, trsvcid=None, subnqn=None,
                                hostaddr=None, hostsvcid=None):
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
        hostaddr: Host address (IP address)
        hostsvcid: transport service ID on host side (port number)
    """
    params = dict()
    params['name'] = name
    if trtype is not None:
        params['trtype'] = trtype
    if traddr is not None:
        params['traddr'] = traddr
    if adrfam is not None:
        params['adrfam'] = adrfam
    if trsvcid is not None:
        params['trsvcid'] = trsvcid
    if subnqn is not None:
        params['subnqn'] = subnqn
    if hostaddr is not None:
        params['hostaddr'] = hostaddr
    if hostsvcid is not None:
        params['hostsvcid'] = hostsvcid
    return client.call('bdev_nvme_detach_controller', params)


def bdev_nvme_reset_controller(client, name, cntlid=None):
    """Reset an NVMe controller or all NVMe controllers in an NVMe bdev controller.
    Args:
        name: controller name
        cntlid: NVMe controller ID (optional)
    """
    params = dict()
    params['name'] = name
    if cntlid is not None:
        params['cntlid'] = cntlid
    return client.call('bdev_nvme_reset_controller', params)


def bdev_nvme_enable_controller(client, name, cntlid=None):
    """Enable an NVMe controller or all NVMe controllers in an NVMe bdev controller.
    Args:
        name: controller name
        cntlid: NVMe controller ID (optional)
    """
    params = dict()
    params['name'] = name
    if cntlid is not None:
        params['cntlid'] = cntlid
    return client.call('bdev_nvme_enable_controller', params)


def bdev_nvme_disable_controller(client, name, cntlid=None):
    """Disable an NVMe controller or all NVMe controllers in an NVMe bdev controller.
    Args:
        name: controller name
        cntlid: NVMe controller ID (optional)
    """
    params = dict()
    params['name'] = name
    if cntlid is not None:
        params['cntlid'] = cntlid
    return client.call('bdev_nvme_disable_controller', params)


def bdev_nvme_start_discovery(client, name, trtype, traddr, adrfam=None, trsvcid=None,
                              hostnqn=None, wait_for_attach=None, ctrlr_loss_timeout_sec=None,
                              reconnect_delay_sec=None, fast_io_fail_timeout_sec=None,
                              attach_timeout_ms=None):
    """Start discovery with the specified discovery subsystem
    Args:
        name: bdev name prefix; "n" + namespace ID will be appended to create unique names
        trtype: transport type ("PCIe", "RDMA", "FC", "TCP")
        traddr: transport address (PCI BDF or IP address)
        adrfam: address family ("IPv4", "IPv6", "IB", or "FC")
        trsvcid: transport service ID (port number for IP-based addresses)
        hostnqn: NQN to connect from (optional)
        wait_for_attach: Wait to complete RPC until all discovered NVM subsystems have attached (optional)
        ctrlr_loss_timeout_sec: Time to wait until ctrlr is reconnected before deleting ctrlr.
        -1 means infinite reconnect retries. 0 means no reconnect retry.
        If reconnect_delay_sec is zero, ctrlr_loss_timeout_sec has to be zero.
        If reconnect_delay_sec is non-zero, ctrlr_loss_timeout_sec has to be -1 or not less than reconnect_delay_sec.
        (optional)
        reconnect_delay_sec: Time to delay a reconnect retry.
        If ctrlr_loss_timeout_sec is zero, reconnect_delay_sec has to be zero.
        If ctrlr_loss_timeout_sec is -1, reconnect_delay_sec has to be non-zero.
        If ctrlr_loss_timeout_sec is not -1 or zero, reconnect_sec has to be non-zero and less than ctrlr_loss_timeout_sec.
        (optional)
        fast_io_fail_timeout_sec: Time to wait until ctrlr is reconnected before failing I/O to ctrlr.
        0 means no such timeout.
        If fast_io_fail_timeout_sec is not zero, it has to be not less than reconnect_delay_sec and less than
        ctrlr_loss_timeout_sec if ctrlr_loss_timeout_sec is not -1. (optional)
        attach_timeout_ms: Time to wait until the discovery and all discovered NVM subsystems are attached (optional)
    """
    params = dict()
    params['name'] = name
    params['trtype'] = trtype
    params['traddr'] = traddr
    if adrfam is not None:
        params['adrfam'] = adrfam
    if trsvcid is not None:
        params['trsvcid'] = trsvcid
    if hostnqn is not None:
        params['hostnqn'] = hostnqn
    if wait_for_attach is not None:
        params['wait_for_attach'] = wait_for_attach
    if ctrlr_loss_timeout_sec is not None:
        params['ctrlr_loss_timeout_sec'] = ctrlr_loss_timeout_sec
    if reconnect_delay_sec is not None:
        params['reconnect_delay_sec'] = reconnect_delay_sec
    if fast_io_fail_timeout_sec is not None:
        params['fast_io_fail_timeout_sec'] = fast_io_fail_timeout_sec
    if attach_timeout_ms is not None:
        params['attach_timeout_ms'] = attach_timeout_ms
    return client.call('bdev_nvme_start_discovery', params)


def bdev_nvme_stop_discovery(client, name):
    """Stop a previously started discovery service
    Args:
        name: name of discovery service to start
    """
    params = dict()
    params['name'] = name
    return client.call('bdev_nvme_stop_discovery', params)


def bdev_nvme_get_discovery_info(client):
    """Get information about the automatic discovery
    """
    return client.call('bdev_nvme_get_discovery_info')


def bdev_nvme_get_io_paths(client, name=None):
    """Display all or the specified NVMe bdev's active I/O paths
    Args:
        name: Name of the NVMe bdev (optional)
    Returns:
        List of active I/O paths
    """
    params = dict()
    if name is not None:
        params['name'] = name
    return client.call('bdev_nvme_get_io_paths', params)


def bdev_nvme_set_preferred_path(client, name, cntlid):
    """Set the preferred I/O path for an NVMe bdev when in multipath mode
    Args:
        name: NVMe bdev name
        cntlid: NVMe-oF controller ID
    """
    params = dict()
    params['name'] = name
    params['cntlid'] = cntlid
    return client.call('bdev_nvme_set_preferred_path', params)


def bdev_nvme_set_multipath_policy(client, name, policy, selector=None, rr_min_io=None):
    """Set multipath policy of the NVMe bdev
    Args:
        name: NVMe bdev name
        policy: Multipath policy (active_passive or active_active)
        selector: Multipath selector (round_robin, queue_depth)
        rr_min_io: Number of IO to route to a path before switching to another one (optional)
    """
    params = dict()
    params['name'] = name
    params['policy'] = policy
    if selector is not None:
        params['selector'] = selector
    if rr_min_io is not None:
        params['rr_min_io'] = rr_min_io
    return client.call('bdev_nvme_set_multipath_policy', params)


def bdev_nvme_get_path_iostat(client, name):
    """Get I/O statistics for IO paths of the block device.
    Args:
        name: bdev name to query
    Returns:
        I/O statistics for IO paths of the requested block device.
    """
    params = dict()
    params['name'] = name
    return client.call('bdev_nvme_get_path_iostat', params)


def bdev_nvme_cuse_register(client, name):
    """Register CUSE devices on NVMe controller.
    Args:
        name: Name of the operating NVMe controller
    """
    params = dict()
    params['name'] = name
    return client.call('bdev_nvme_cuse_register', params)


def bdev_nvme_cuse_unregister(client, name):
    """Unregister CUSE devices on NVMe controller.
    Args:
        name: Name of the operating NVMe controller
    """
    params = dict()
    params['name'] = name
    return client.call('bdev_nvme_cuse_unregister', params)


def bdev_nvme_set_keys(client, name, dhchap_key=None, dhchap_ctrlr_key=None):
    """Set DH-HMAC-CHAP keys and force (re)authentication on all connected qpairs.
    Args:
        name: name of the NVMe controller
        dhchap_key: DH-HMAC-CHAP key name
        dhchap_ctrlr_key: DH-HMAC-CHAP controller key name
    """
    params = {'name': name}
    if dhchap_key is not None:
        params['dhchap_key'] = dhchap_key
    if dhchap_ctrlr_key is not None:
        params['dhchap_ctrlr_key'] = dhchap_ctrlr_key
    return client.call('bdev_nvme_set_keys', params)


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
    params = dict()
    params['name'] = name
    params['base_bdev'] = base_bdev
    params['zone_capacity'] = zone_capacity
    params['optimal_open_zones'] = optimal_open_zones
    return client.call('bdev_zone_block_create', params)


def bdev_zone_block_delete(client, name):
    """Remove block zone bdev from the system.
    Args:
        name: name of block zone bdev to delete
    """
    params = dict()
    params['name'] = name
    return client.call('bdev_zone_block_delete', params)


def bdev_rbd_register_cluster(client, name=None, user_id=None, config_param=None, config_file=None, key_file=None, core_mask=None):
    """Create a Rados Cluster object of the Ceph RBD backend.
    Args:
        name: name of Rados Cluster
        user_id: Ceph user name (optional)
        config_param: map of config keys to values (optional)
        config_file: file path of Ceph configuration file (optional)
        key_file: file path of Ceph key file (optional)
        core_mask: core mask for librbd IO context threads (optional)
    Returns:
        Name of registered Rados Cluster object.
    """
    params = dict()
    if name is not None:
        params['name'] = name
    if user_id is not None:
        params['user_id'] = user_id
    if config_param is not None:
        params['config_param'] = config_param
    if config_file is not None:
        params['config_file'] = config_file
    if key_file is not None:
        params['key_file'] = key_file
    if core_mask is not None:
        params['core_mask'] = core_mask
    return client.call('bdev_rbd_register_cluster', params)


def bdev_rbd_unregister_cluster(client, name):
    """Remove Rados cluster object from the system.
    Args:
        name: name of Rados cluster object to unregister
    """
    params = dict()
    params['name'] = name
    return client.call('bdev_rbd_unregister_cluster', params)

def bdev_rbd_wait_for_latest_osdmap(client, name):
    """Wait for latest osd map on cluster identified by name.

    Args:
        name: name of Rados cluster
    """
    params = {'name': name}
    return client.call('bdev_rbd_wait_for_latest_osdmap', params)


def bdev_rbd_get_clusters_info(client, name=None):
    """Get the cluster(s) info
    Args:
        name: name of Rados cluster object to query (optional; if omitted, query all clusters)
    Returns:
        List of registered Rados cluster information objects.
    """
    params = dict()
    if name is not None:
        params['name'] = name
    return client.call('bdev_rbd_get_clusters_info', params)


def bdev_rbd_create(client, pool_name, rbd_name, block_size, name=None, user_id=None, config=None, cluster_name=None, uuid=None):
    """Create a Ceph RBD block device.
    Args:
        pool_name: Ceph RBD pool name
        rbd_name: Ceph RBD image name
        block_size: block size of RBD volume
        name: name of block device (optional)
        user_id: Ceph user name (optional)
        config: map of config keys to values (optional)
        cluster_name: Name to identify Rados cluster (optional)
        uuid: UUID of block device (optional)
    Returns:
        Name of created block device.
    """
    params = dict()
    params['pool_name'] = pool_name
    params['rbd_name'] = rbd_name
    params['block_size'] = block_size
    if name is not None:
        params['name'] = name
    if user_id is not None:
        params['user_id'] = user_id
    if config is not None:
        params['config'] = config
    if cluster_name is not None:
        params['cluster_name'] = cluster_name
    else:
        print("WARNING:bdev_rbd_create should be used with specifying -c to have a cluster name after bdev_rbd_register_cluster.")
    if uuid is not None:
        params['uuid'] = uuid
    return client.call('bdev_rbd_create', params)


def bdev_rbd_delete(client, name):
    """Remove rbd bdev from the system.
    Args:
        name: name of rbd bdev to delete
    """
    params = dict()
    params['name'] = name
    return client.call('bdev_rbd_delete', params)


def bdev_rbd_resize(client, name, new_size):
    """Resize rbd bdev in the system.
    Args:
        name: name of rbd bdev to resize
        new_size: new bdev size of resize operation. The unit is MiB
    """
    params = dict()
    params['name'] = name
    params['new_size'] = new_size
    return client.call('bdev_rbd_resize', params)


def bdev_error_create(client, base_name, uuid=None):
    """Construct an error injection block device.
    Args:
        base_name: base bdev name
        uuid: UUID for this bdev (optional)
    """
    params = dict()
    params['base_name'] = base_name
    if uuid is not None:
        params['uuid'] = uuid
    return client.call('bdev_error_create', params)


def bdev_delay_create(client, base_bdev_name, name, avg_read_latency, p99_read_latency, avg_write_latency, p99_write_latency, uuid=None):
    """Construct a delay block device.
    Args:
        base_bdev_name: name of the existing bdev
        name: name of block device
        avg_read_latency: complete 99% of read ops with this delay
        p99_read_latency: complete 1% of read ops with this delay
        avg_write_latency: complete 99% of write ops with this delay
        p99_write_latency: complete 1% of write ops with this delay
        uuid: UUID of block device (optional)
    Returns:
        Name of created block device.
    """
    params = dict()
    params['base_bdev_name'] = base_bdev_name
    params['name'] = name
    params['avg_read_latency'] = avg_read_latency
    params['p99_read_latency'] = p99_read_latency
    params['avg_write_latency'] = avg_write_latency
    params['p99_write_latency'] = p99_write_latency
    if uuid is not None:
        params['uuid'] = uuid
    return client.call('bdev_delay_create', params)


def bdev_delay_delete(client, name):
    """Remove delay bdev from the system.
    Args:
        name: name of delay bdev to delete
    """
    params = dict()
    params['name'] = name
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
    params = dict()
    params['delay_bdev_name'] = delay_bdev_name
    params['latency_type'] = latency_type
    params['latency_us'] = latency_us
    return client.call('bdev_delay_update_latency', params)


def bdev_error_delete(client, name):
    """Remove error bdev from the system.
    Args:
        bdev_name: name of error bdev to delete
    """
    params = dict()
    params['name'] = name
    return client.call('bdev_error_delete', params)


def bdev_iscsi_set_options(client, timeout_sec=None):
    """Set options for the bdev iscsi.
    Args:
        timeout_sec: Timeout for command, in seconds, if 0, don't track timeout
    """
    params = dict()
    if timeout_sec is not None:
        params['timeout_sec'] = timeout_sec
    return client.call('bdev_iscsi_set_options', params)


def bdev_iscsi_create(client, name, url, initiator_iqn):
    """Construct an iSCSI block device.
    Args:
        name: name of block device
        url: iSCSI URL
        initiator_iqn: IQN name to be used by initiator
    Returns:
        Name of created block device.
    """
    params = dict()
    params['name'] = name
    params['url'] = url
    params['initiator_iqn'] = initiator_iqn
    return client.call('bdev_iscsi_create', params)


def bdev_iscsi_delete(client, name):
    """Remove iSCSI bdev from the system.
    Args:
        bdev_name: name of iSCSI bdev to delete
    """
    params = dict()
    params['name'] = name
    return client.call('bdev_iscsi_delete', params)


def bdev_passthru_create(client, base_bdev_name, name, uuid=None):
    """Construct a pass-through block device.
    Args:
        base_bdev_name: name of the existing bdev
        name: name of block device
        uuid: UUID of block device (optional)
    Returns:
        Name of created block device.
    """
    params = dict()
    params['base_bdev_name'] = base_bdev_name
    params['name'] = name
    if uuid is not None:
        params['uuid'] = uuid
    return client.call('bdev_passthru_create', params)


def bdev_passthru_delete(client, name):
    """Remove pass through bdev from the system.
    Args:
        name: name of pass through bdev to delete
    """
    params = dict()
    params['name'] = name
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
    params = dict()
    params['nvme_ctrlr_name'] = nvme_ctrlr_name
    params['nsid'] = nsid
    params['locking_range_id'] = locking_range_id
    params['range_start'] = range_start
    params['range_length'] = range_length
    params['password'] = password
    return client.call('bdev_opal_create', params)


def bdev_opal_get_info(client, bdev_name, password):
    """Get opal locking range info.
    Args:
        bdev_name: name of opal vbdev to get info
        password: admin password
    Returns:
        Locking range info.
    """
    params = dict()
    params['bdev_name'] = bdev_name
    params['password'] = password
    return client.call('bdev_opal_get_info', params)


def bdev_opal_delete(client, bdev_name, password):
    """Delete opal virtual bdev from the system.
    Args:
        bdev_name: name of opal vbdev to delete
        password: admin password of base nvme bdev
    """
    params = dict()
    params['bdev_name'] = bdev_name
    params['password'] = password
    return client.call('bdev_opal_delete', params)


def bdev_opal_new_user(client, bdev_name, admin_password, user_id, user_password):
    """Add a user to opal bdev who can set lock state for this bdev.
    Args:
        bdev_name: name of opal vbdev
        admin_password: admin password
        user_id: ID of the user who will be added to this opal bdev
        user_password: password set for this user
    """
    params = dict()
    params['bdev_name'] = bdev_name
    params['admin_password'] = admin_password
    params['user_id'] = user_id
    params['user_password'] = user_password
    return client.call('bdev_opal_new_user', params)


def bdev_opal_set_lock_state(client, bdev_name, user_id, password, lock_state):
    """set lock state for an opal bdev.
    Args:
        bdev_name: name of opal vbdev
        user_id: ID of the user who will set lock state
        password: password of the user
        lock_state: lock state to set
    """
    params = dict()
    params['bdev_name'] = bdev_name
    params['user_id'] = user_id
    params['password'] = password
    params['lock_state'] = lock_state
    return client.call('bdev_opal_set_lock_state', params)


def bdev_split_create(client, base_bdev, split_count, split_size_mb=None):
    """Create split block devices from a base bdev.
    Args:
        base_bdev: name of bdev to split
        split_count: number of split bdevs to create
        split_size_mb: size of each split volume in MiB (optional)
    Returns:
        List of created block devices.
    """
    params = dict()
    params['base_bdev'] = base_bdev
    params['split_count'] = split_count
    if split_size_mb is not None:
        params['split_size_mb'] = split_size_mb
    return client.call('bdev_split_create', params)


def bdev_split_delete(client, base_bdev):
    """Delete split block devices.
    Args:
        base_bdev: name of previously split bdev
    """
    params = dict()
    params['base_bdev'] = base_bdev
    return client.call('bdev_split_delete', params)


def bdev_ftl_create(client, name, base_bdev, cache, **kwargs):
    """Construct FTL bdev
    Args:
        name: name of the bdev
        base_bdev: name of the base bdev
        cache: name of the cache device
        kwargs: optional parameters
    """
    params = dict()
    params['name'] = name
    params['base_bdev'] = base_bdev
    params['cache'] = cache
    for key, value in kwargs.items():
        if value is not None:
            params[key] = value
    return client.call('bdev_ftl_create', params)


def bdev_ftl_load(client, name, base_bdev, cache, **kwargs):
    """Load FTL bdev
    Args:
        name: name of the bdev
        base_bdev: name of the base bdev
        cache: Name of the cache device
        kwargs: optional parameters
    """
    params = dict()
    params['name'] = name
    params['base_bdev'] = base_bdev
    params['cache'] = cache
    for key, value in kwargs.items():
        if value is not None:
            params[key] = value
    return client.call('bdev_ftl_load', params)


def bdev_ftl_unload(client, name, fast_shutdown=None):
    """Unload FTL bdev
    Args:
        name: name of the bdev
        fast_shutdown: When set FTL will minimize persisted data during deletion and rely on shared memory during next load
    """
    params = dict()
    params['name'] = name
    if fast_shutdown is not None:
        params['fast_shutdown'] = fast_shutdown
    return client.call('bdev_ftl_unload', params)


def bdev_ftl_delete(client, name, fast_shutdown=None):
    """Delete FTL bdev
    Args:
        name: name of the bdev
        fast_shutdown: When set FTL will minimize persisted data during deletion and rely on shared memory during next load
    """
    params = dict()
    params['name'] = name
    if fast_shutdown is not None:
        params['fast_shutdown'] = fast_shutdown
    return client.call('bdev_ftl_delete', params)


def bdev_ftl_unmap(client, name, lba, num_blocks):
    """FTL unmap
    Args:
        name: name of the bdev
        lba: starting lba to be unmapped
        num_blocks: number of blocks to unmap
    """
    params = dict()
    params['name'] = name
    params['lba'] = lba
    params['num_blocks'] = num_blocks
    return client.call('bdev_ftl_unmap', params)


def bdev_ftl_get_stats(client, name):
    """get FTL stats
    Args:
        name: name of the bdev
    """
    params = dict()
    params['name'] = name
    return client.call('bdev_ftl_get_stats', params)


def bdev_ftl_get_properties(client, name):
    """Get FTL properties
    Args:
        name: name of the bdev
    """
    params = dict()
    params['name'] = name
    return client.call('bdev_ftl_get_properties', params)


def bdev_ftl_set_property(client, name, ftl_property, value):
    """Set FTL property
    Args:
        name: name of the bdev
        ftl_property: name of the property to be set
        value: The new value of the updated property
    """
    params = dict()
    params['name'] = name
    params['ftl_property'] = ftl_property
    params['value'] = value
    return client.call('bdev_ftl_set_property', params)


def bdev_get_bdevs(client, name=None, timeout=None):
    """Get information about block devices.
    Args:
        name: bdev name to query (optional; if omitted, query all bdevs)
        timeout: time in ms to wait for the bdev with specified name to appear
    Returns:
        List of bdev information objects.
    """
    params = dict()
    if name is not None:
        params['name'] = name
    if timeout is not None:
        params['timeout'] = timeout
    return client.call('bdev_get_bdevs', params)


def bdev_get_iostat(client, name=None, per_channel=None):
    """Get I/O statistics for block devices.
    Args:
        name: bdev name to query (optional; if omitted, query all bdevs)
        per_channel: display per channel IO stats for specified bdev
    Returns:
        I/O statistics for the requested block devices.
    """
    params = dict()
    if name is not None:
        params['name'] = name
    if per_channel is not None:
        params['per_channel'] = per_channel
    return client.call('bdev_get_iostat', params)


def bdev_reset_iostat(client, name=None, mode=None):
    """Reset I/O statistics for block devices.
    Args:
        name: bdev name to reset (optional; if omitted, reset all bdevs)
        mode: mode to reset: all, maxmin (optional: if omitted, reset all fields)
    """
    params = dict()
    if name is not None:
        params['name'] = name
    if mode is not None:
        params['mode'] = mode
    return client.call('bdev_reset_iostat', params)


def bdev_enable_histogram(client, name, enable, opc):
    """Control whether histogram is enabled for specified bdev.
    Args:
        name: name of bdev
        enable: Enable or disable histogram on specified device
        opc: name of io_type (optional)
    """
    params = dict()
    params['name'] = name
    params['enable'] = enable
    if opc:
        params['opc'] = opc
    return client.call('bdev_enable_histogram', params)


def bdev_get_histogram(client, name):
    """Get histogram for specified bdev.
    Args:
        name: name of bdev
    """
    params = dict()
    params['name'] = name
    return client.call('bdev_get_histogram', params)


def bdev_error_inject_error(client, name, io_type, error_type, num=None,
                            queue_depth=None, corrupt_offset=None, corrupt_value=None):
    """Inject an error via an error bdev.
    Args:
        name: name of error bdev
        io_type: one of "clear", "read", "write", "unmap", "flush", or "all"
        error_type: one of "failure", "pending", "corrupt_data" or "nomem"
        num: number of commands to fail
        queue_depth: the queue depth at which to trigger the error
        corrupt_offset: offset in bytes to xor with corrupt_value
        corrupt_value: value for xor (1-255, 0 is invalid)
    """
    params = dict()
    params['name'] = name
    params['io_type'] = io_type
    params['error_type'] = error_type
    if num is not None:
        params['num'] = num
    if queue_depth is not None:
        params['queue_depth'] = queue_depth
    if corrupt_offset is not None:
        params['corrupt_offset'] = corrupt_offset
    if corrupt_value is not None:
        params['corrupt_value'] = corrupt_value
    return client.call('bdev_error_inject_error', params)


def bdev_set_qd_sampling_period(client, name, period):
    """Enable queue depth tracking on a specified bdev.
    Args:
        name: name of a bdev on which to track queue depth.
        period: period (in microseconds) at which to update the queue depth reading. If set to 0, polling will be disabled.
    """
    params = dict()
    params['name'] = name
    params['period'] = period
    return client.call('bdev_set_qd_sampling_period', params)


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
    params = dict()
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


def bdev_nvme_apply_firmware(client, bdev_name, filename):
    """Download and commit firmware to NVMe device.
    Args:
        bdev_name: name of NVMe block device
        filename: filename of the firmware to download
    """
    params = dict()
    params['bdev_name'] = bdev_name
    params['filename'] = filename
    return client.call('bdev_nvme_apply_firmware', params)


def bdev_nvme_get_transport_statistics(client):
    """Get bdev_nvme poll group transport statistics"""
    return client.call('bdev_nvme_get_transport_statistics')


def bdev_nvme_get_controller_health_info(client, name):
    """Display health log of the required NVMe bdev controller.
    Args:
        name: name of the required NVMe bdev controller
    Returns:
        Health log for the requested NVMe bdev controller.
    """
    params = dict()
    params['name'] = name
    return client.call('bdev_nvme_get_controller_health_info', params)


def bdev_daos_create(client, num_blocks, block_size, pool, cont, name, oclass=None, uuid=None):
    """Construct DAOS block device.
    Args:
        num_blocks: size of block device in blocks
        block_size: block size of device; must be a power of 2 and at least 512
        pool: UUID of DAOS pool
        cont: UUID of DAOS container
        name: name of block device (also the name of the backend file on DAOS DFS)
        oclass: DAOS object class (optional)
        uuid: UUID of block device (optional)
    Returns:
        Name of created block device.
    """
    params = dict()
    params['num_blocks'] = num_blocks
    params['block_size'] = block_size
    params['pool'] = pool
    params['cont'] = cont
    params['name'] = name
    if oclass is not None:
        params['oclass'] = oclass
    if uuid is not None:
        params['uuid'] = uuid
    return client.call('bdev_daos_create', params)


def bdev_daos_delete(client, name):
    """Delete DAOS block device.
    Args:
        bdev_name: name of DAOS bdev to delete
    """
    params = dict()
    params['name'] = name
    return client.call('bdev_daos_delete', params)


def bdev_daos_resize(client, name, new_size):
    """Resize DAOS bdev in the system.
    Args:
        name: name of DAOS bdev to resize
        new_size: new bdev size of resize operation. The unit is MiB
    """
    params = dict()
    params['name'] = name
    params['new_size'] = new_size
    return client.call('bdev_daos_resize', params)


def bdev_nvme_start_mdns_discovery(client, name, svcname, hostnqn=None):
    """Start discovery with mDNS
    Args:
        name: bdev name prefix; "n" + unique seqno + namespace ID will be appended to create unique names
        svcname: service to discover ("_nvme-disc._tcp")
        hostnqn: NQN to connect from (optional)
    """
    params = dict()
    params['name'] = name
    params['svcname'] = svcname
    if hostnqn is not None:
        params['hostnqn'] = hostnqn
    return client.call('bdev_nvme_start_mdns_discovery', params)


def bdev_nvme_stop_mdns_discovery(client, name):
    """Stop a previously started mdns discovery service
    Args:
        name: name of the discovery service to stop
    """
    params = dict()
    params['name'] = name
    return client.call('bdev_nvme_stop_mdns_discovery', params)


def bdev_nvme_get_mdns_discovery_info(client):
    """Get information about the automatic mdns discovery
    """
    return client.call('bdev_nvme_get_mdns_discovery_info')
