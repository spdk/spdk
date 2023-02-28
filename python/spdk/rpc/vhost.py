#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2017 Intel Corporation.
#  All rights reserved.

from .cmd_parser import *


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


def virtio_blk_get_transports(client, name=None):
    """Get list of virtio-blk transports.
    Args:
        name: name of the virtio-blk transport (optional).

    Returns:
        List of virtio-blk transport objects.
    """

    params = {}

    if name:
        params['name'] = name

    return client.call('virtio_blk_get_transports', params)


def virtio_blk_create_transport(client, **params):
    """Create virtio blk transport.
    Args:
        name: transport name
    """
    strip_globals(params)
    remove_null(params)

    return client.call('virtio_blk_create_transport', params)


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


def vhost_create_blk_controller(client, **params):
    """Create vhost BLK controller.
    Args:
        ctrlr: controller name
        dev_name: device name to add to controller
        cpumask: cpu mask for this controller
        transport: virtio blk transport name (default: vhost_user_blk)
        readonly: set controller as read-only
        packed_ring: support controller packed_ring
        packed_ring_recovery: enable packed ring live recovery
    """
    strip_globals(params)
    remove_null(params)

    return client.call('vhost_create_blk_controller', params)


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


def vhost_delete_controller(client, ctrlr):
    """Delete vhost controller from configuration.
    Args:
        ctrlr: controller name to remove
    """
    params = {'ctrlr': ctrlr}
    return client.call('vhost_delete_controller', params)


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


def bdev_virtio_detach_controller(client, name):
    """Remove a Virtio device
    This will delete all bdevs exposed by this device.
    Args:
        name: virtio device name
    """
    params = {'name': name}
    return client.call('bdev_virtio_detach_controller', params)


def bdev_virtio_scsi_get_devices(client):
    """Get list of virtio scsi devices."""
    return client.call('bdev_virtio_scsi_get_devices')


def bdev_virtio_blk_set_hotplug(client, enable, period_us=None):
    """Set options for the bdev virtio blk. This is startup command.

    Args:
       enable: True to enable hotplug, False to disable.
       period_us: how often the hotplug is processed for insert and remove events. Set 0 to reset to default. (optional)
    """
    params = {'enable': enable}

    if period_us:
        params['period_us'] = period_us

    return client.call('bdev_virtio_blk_set_hotplug', params)
