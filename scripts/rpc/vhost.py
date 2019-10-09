from .helpers import deprecated_alias


@deprecated_alias('set_vhost_controller_coalescing')
def vhost_controller_set_coalescing(client, ctrlr, delay_base_us, iops_threshold):
    """Set coalescing for vhost controller.
    Args:
        ctrlr: controller name
        delay_base_us: base delay time
        iops_threshold: IOPS threshold when coalescing is enabled
    """
    params = {
        'ctrlr': ctrlr,
        'delay_base_us': delay_base_us,
        'iops_threshold': iops_threshold,
    }
    return client.call('vhost_controller_set_coalescing', params)


@deprecated_alias('construct_vhost_scsi_controller')
def vhost_create_scsi_controller(client, ctrlr, cpumask=None):
    """Create a vhost scsi controller.
    Args:
        ctrlr: controller name
        cpumask: cpu mask for this controller
    """
    params = {'ctrlr': ctrlr}

    if cpumask:
        params['cpumask'] = cpumask

    return client.call('vhost_create_scsi_controller', params)


@deprecated_alias('add_vhost_scsi_lun')
def vhost_scsi_controller_add_target(client, ctrlr, scsi_target_num, bdev_name):
    """Add LUN to vhost scsi controller target.
    Args:
        ctrlr: controller name
        scsi_target_num: target number to use
        bdev_name: name of bdev to add to target
    """
    params = {
        'ctrlr': ctrlr,
        'scsi_target_num': scsi_target_num,
        'bdev_name': bdev_name,
    }
    return client.call('vhost_scsi_controller_add_target', params)


@deprecated_alias('remove_vhost_scsi_target')
def vhost_scsi_controller_remove_target(client, ctrlr, scsi_target_num):
    """Remove target from vhost scsi controller.
    Args:
        ctrlr: controller name to remove target from
        scsi_target_num: number of target to remove from controller
    """
    params = {
        'ctrlr': ctrlr,
        'scsi_target_num': scsi_target_num
    }
    return client.call('vhost_scsi_controller_remove_target', params)


@deprecated_alias('construct_vhost_nvme_controller')
def vhost_create_nvme_controller(client, ctrlr, io_queues, cpumask=None):
    """Construct vhost NVMe controller.
    Args:
        ctrlr: controller name
        io_queues: number of IO queues for the controller
        cpumask: cpu mask for this controller
    """
    params = {
        'ctrlr': ctrlr,
        'io_queues': io_queues
    }

    if cpumask:
        params['cpumask'] = cpumask

    return client.call('vhost_create_nvme_controller', params)


@deprecated_alias('add_vhost_nvme_ns')
def vhost_nvme_controller_add_ns(client, ctrlr, bdev_name):
    """Add namespace to vhost nvme controller.
    Args:
        ctrlr: controller name where to add a namespace
        bdev_name: block device name for a new namespace
    """
    params = {
        'ctrlr': ctrlr,
        'bdev_name': bdev_name,
    }

    return client.call('vhost_nvme_controller_add_ns', params)


@deprecated_alias('construct_vhost_blk_controller')
def vhost_create_blk_controller(client, ctrlr, dev_name, cpumask=None, readonly=None):
    """Create vhost BLK controller.
    Args:
        ctrlr: controller name
        dev_name: device name to add to controller
        cpumask: cpu mask for this controller
        readonly: set controller as read-only
    """
    params = {
        'ctrlr': ctrlr,
        'dev_name': dev_name,
    }
    if cpumask:
        params['cpumask'] = cpumask
    if readonly:
        params['readonly'] = readonly
    return client.call('vhost_create_blk_controller', params)


@deprecated_alias('get_vhost_controllers')
def vhost_get_controllers(client, name=None):
    """Get information about configured vhost controllers.

    Args:
        name: controller name to query (optional; if omitted, query all controllers)

    Returns:
        List of vhost controllers.
    """
    params = {}
    if name:
        params['name'] = name
    return client.call('vhost_get_controllers', params)


@deprecated_alias('remove_vhost_controller')
def vhost_delete_controller(client, ctrlr):
    """Delete vhost controller from configuration.
    Args:
        ctrlr: controller name to remove
    """
    params = {'ctrlr': ctrlr}
    return client.call('vhost_delete_controller', params)


@deprecated_alias('construct_virtio_dev')
def bdev_virtio_attach_controller(client, name, trtype, traddr, dev_type, vq_count=None, vq_size=None):
    """Attaches virtio controller using
    provided transport type and device type.
    This will also create bdevs for any block
    devices connected to that controller.
    Args:
        name: name base for new created bdevs
        trtype: virtio target transport type: pci or user
        traddr: transport type specific target address: e.g. UNIX
                domain socket path or BDF
        dev_type: device type: blk or scsi
        vq_count: number of virtual queues to be used
        vq_size: size of each queue
    """
    params = {
        'name': name,
        'trtype': trtype,
        'traddr': traddr,
        'dev_type': dev_type
    }
    if vq_count:
        params['vq_count'] = vq_count
    if vq_size:
        params['vq_size'] = vq_size
    return client.call('bdev_virtio_attach_controller', params)


@deprecated_alias('remove_virtio_bdev ')
def bdev_virtio_detach_controller(client, name):
    """Remove a Virtio device
    This will delete all bdevs exposed by this device.
    Args:
        name: virtio device name
    """
    params = {'name': name}
    return client.call('bdev_virtio_detach_controller', params)


@deprecated_alias('get_virtio_scsi_devs')
def bdev_virtio_scsi_get_devices(client):
    """Get list of virtio scsi devices."""
    return client.call('bdev_virtio_scsi_get_devices')
