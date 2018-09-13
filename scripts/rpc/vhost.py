def set_vhost_controller_coalescing(client, ctrlr, delay_base_us, iops_threshold):
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
    return client.call('set_vhost_controller_coalescing', params)


def construct_vhost_scsi_controller(client, ctrlr, cpumask=None):
    """Construct a vhost scsi controller.
    Args:
        ctrlr: controller name
        cpumask: cpu mask for this controller
    """
    params = {'ctrlr': ctrlr}

    if cpumask:
        params['cpumask'] = cpumask

    return client.call('construct_vhost_scsi_controller', params)


def add_vhost_scsi_lun(client, ctrlr, scsi_target_num, bdev_name):
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
    return client.call('add_vhost_scsi_lun', params)


def remove_vhost_scsi_target(client, ctrlr, scsi_target_num):
    """Remove target from vhost scsi controller.
    Args:
        ctrlr: controller name to remove target from
        scsi_target_num: number of target to remove from controller
    """
    params = {
        'ctrlr': ctrlr,
        'scsi_target_num': scsi_target_num
    }
    return client.call('remove_vhost_scsi_target', params)


def construct_vhost_nvme_controller(client, ctrlr, io_queues, cpumask=None):
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

    return client.call('construct_vhost_nvme_controller', params)


def add_vhost_nvme_ns(client, ctrlr, bdev_name):
    """Add namespace to vhost nvme controller.
    Args:
        ctrlr: controller name where to add a namespace
        bdev_name: block device name for a new namespace
    """
    params = {
        'ctrlr': ctrlr,
        'bdev_name': bdev_name,
    }

    return client.call('add_vhost_nvme_ns', params)


def construct_vhost_blk_controller(client, ctrlr, dev_name, cpumask=None, readonly=None):
    """Construct vhost BLK controller.
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
    return client.call('construct_vhost_blk_controller', params)


def get_vhost_controllers(client, name=None):
    """Get information about configured vhost controllers.

    Args:
        name: controller name to query (optional; if omitted, query all controllers)

    Returns:
        List of vhost controllers.
    """
    params = {}
    if name:
        params['name'] = name
    return client.call('get_vhost_controllers', params)


def remove_vhost_controller(client, ctrlr):
    """Remove vhost controller from configuration.
    Args:
        ctrlr: controller name to remove
    """
    params = {'ctrlr': ctrlr}
    return client.call('remove_vhost_controller', params)


def construct_virtio_dev(client, name, trtype, traddr, dev_type, vq_count=None, vq_size=None):
    """Construct new virtio device using provided
    transport type and device type.
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
    return client.call('construct_virtio_dev', params)


def construct_virtio_user_scsi_bdev(client, path, name, vq_count=None, vq_size=None):
    """Connect to virtio user scsi device.
    Args:
        path: path to Virtio SCSI socket
        name: use this name as base instead of 'VirtioScsiN'
        vq_count: number of virtual queues to be used
        vq_size: size of each queue
    """
    params = {
        'path': path,
        'name': name,
    }
    if vq_count:
        params['vq_count'] = vq_count
    if vq_size:
        params['vq_size'] = vq_size
    return client.call('construct_virtio_user_scsi_bdev', params)


def construct_virtio_pci_scsi_bdev(client, pci_address, name):
    """Create a Virtio SCSI device from a virtio-pci device.
    Args:
        pci_address: PCI address in domain:bus:device.function format or
               domain.bus.device.function format
        name: Name for the virtio device. It will be inhereted by all created
               bdevs, which are named n the following format:
               <name>t<target_id>
    """
    params = {
        'pci_address': pci_address,
        'name': name,
    }
    return client.call('construct_virtio_pci_scsi_bdev', params)


def remove_virtio_scsi_bdev(client, name):
    """Remove a Virtio-SCSI device
    This will delete all bdevs exposed by this device.
    Args:
        name: virtio device name
    """
    params = {'name': name}
    return client.call('remove_virtio_scsi_bdev', params)


def remove_virtio_bdev(client, name):
    """Remove a Virtio device
    This will delete all bdevs exposed by this device.
    Args:
        name: virtio device name
    """
    params = {'name': name}
    return client.call('remove_virtio_bdev', params)


def get_virtio_scsi_devs(client):
    """Get list of virtio scsi devices."""
    return client.call('get_virtio_scsi_devs')


def construct_virtio_user_blk_bdev(client, path, name, vq_count=None, vq_size=None):
    """Connect to virtio user BLK device.
    Args:
        path: path to Virtio BLK socket
        name: use this name as base instead of 'VirtioScsiN'
        vq_count: number of virtual queues to be used
        vq_size: size of each queue
    """
    params = {
        'path': path,
        'name': name,
    }
    if vq_count:
        params['vq_count'] = vq_count
    if vq_size:
        params['vq_size'] = vq_size
    return client.call('construct_virtio_user_blk_bdev', params)


def construct_virtio_pci_blk_bdev(client, pci_address, name):
    """Create a Virtio Blk device from a virtio-pci device.
    Args:
        pci_address: PCI address in domain:bus:device.function format or
               domain.bus.device.function format
        name: name for the blk device
    """
    params = {
        'pci_address': pci_address,
        'name': name,
    }
    return client.call('construct_virtio_pci_blk_bdev', params)
