#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2018 Intel Corporation.
#  All rights reserved.


def bdev_nvme_send_cmd(client, name, cmd_type, data_direction, cmdbuf,
                       data=None, metadata=None,
                       data_len=None, metadata_len=None,
                       timeout_ms=None):
    """Send one NVMe command

    Args:
        name: Name of the operating NVMe controller
        cmd_type: Type of nvme cmd. Valid values are: admin, io
        data_direction: Direction of data transfer. Valid values are: c2h, h2c
        cmdbuf: NVMe command encoded by base64 urlsafe
        data: Data transferring to controller from host, encoded by base64 urlsafe
        metadata: metadata transferring to controller from host, encoded by base64 urlsafe
        data_length: Data length required to transfer from controller to host
        metadata_length: Metadata length required to transfer from controller to host
        timeout-ms: Command execution timeout value, in milliseconds, if 0, don't track timeout

    Returns:
        NVMe completion queue entry, requested data and metadata, all are encoded by base64 urlsafe.
    """
    params = {'name': name,
              'cmd_type': cmd_type,
              'data_direction': data_direction,
              'cmdbuf': cmdbuf}

    if data:
        params['data'] = data
    if metadata:
        params['metadata'] = metadata
    if data_len:
        params['data_len'] = data_len
    if metadata_len:
        params['metadata_len'] = metadata_len
    if timeout_ms:
        params['timeout_ms'] = timeout_ms

    return client.call('bdev_nvme_send_cmd', params)


def bdev_nvme_get_controllers(client, name=None):
    """Get information about NVMe controllers.

    Args:
        name: NVMe controller name to query (optional; if omitted, query all NVMe controllers)

    Returns:
        List of NVMe controller information objects.
    """
    params = {}
    if name:
        params['name'] = name
    return client.call('bdev_nvme_get_controllers', params)


def bdev_nvme_opal_init(client, nvme_ctrlr_name, password):
    """Init nvme opal. Take ownership and activate

    Args:
        nvme_ctrlr_name: name of nvme ctrlr
        password: password to init opal
    """
    params = {
        'nvme_ctrlr_name': nvme_ctrlr_name,
        'password': password,
    }

    return client.call('bdev_nvme_opal_init', params)


def bdev_nvme_opal_revert(client, nvme_ctrlr_name, password):
    """Revert opal to default factory settings. Erase all data.

    Args:
        nvme_ctrlr_name: name of nvme ctrlr
        password: password
    """
    params = {
        'nvme_ctrlr_name': nvme_ctrlr_name,
        'password': password,
    }

    return client.call('bdev_nvme_opal_revert', params)


def bdev_nvme_add_error_injection(client, name, opc, cmd_type, do_not_submit, timeout_in_us,
                                  err_count, sct, sc):
    """Add error injection

    Args:
        name: Name of the operating NVMe controller
        opc: Opcode of the NVMe command
        cmd_type: Type of NVMe command. Valid values are: admin, io
        do_not_submit: Do not submit commands to the controller
        timeout_in_us: Wait specified microseconds when do_not_submit is true
        err_count: Number of matching NVMe commands to inject errors
        sct: NVMe status code type
        sc: NVMe status code

    Returns:
        True on success, RPC error otherwise
    """
    params = {'name': name,
              'opc': opc,
              'cmd_type': cmd_type}

    if do_not_submit:
        params['do_not_submit'] = do_not_submit
    if timeout_in_us:
        params['timeout_in_us'] = timeout_in_us
    if err_count:
        params['err_count'] = err_count
    if sct:
        params['sct'] = sct
    if sc:
        params['sc'] = sc

    return client.call('bdev_nvme_add_error_injection', params)


def bdev_nvme_remove_error_injection(client, name, opc, cmd_type):
    """Remove error injection

    Args:
        name: Name of the operating NVMe controller
        opc: Opcode of the NVMe command
        cmd_type: Type of NVMe command. Valid values are: admin, io

    Returns:
        True on success, RPC error otherwise
    """
    params = {'name': name,
              'opc': opc,
              'cmd_type': cmd_type}

    return client.call('bdev_nvme_remove_error_injection', params)
