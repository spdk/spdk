from .helpers import deprecated_alias


@deprecated_alias('send_nvme_cmd')
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


@deprecated_alias('get_nvme_controllers')
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
    
    
def nvme_controllers_error_injection(client, opcode, do_not_submit, sct, sc, name=None, admin=None, \
        timeout_in_us=None, err_count=None, info=None):
    """Inject an error for the next request with a given opcode.

    Args:
        name: NVMe controller name to inject an error (optional; if omitted, inject to Admin controller)
        admin: if set - error injected for Admin command type, otherwise IO
        opcode: 0x... Opcode for Admin or I/O commands
        do_not_submit: True if matching requests should not be submitted to \
                        the controller, but instead completed manually after \
                        timeout_in_us has expired. False if matching requests \
                        should be submitted to the controller and have their \
                        completion status modified after the controller \
                        completes the request
        timeout_in_us: Wait specified microseconds when do_not_submit is true
        err_count: Number of matching requests to inject errors
        sct: Status code type
        sc: Status code
        info: Show controller information

    Returns:
        Success or failure.
    """
    params = {
        'admin': admin,
        'opcode': opcode,
        'do_not_submit': do_not_submit,
        'sct': sct,
        'sc': sc,
        'info': info
        }
    if name:
        params['name'] = name
    if timeout_in_us:
        params['timeout_in_us'] = timeout_in_us
    if err_count:
        params['err_count'] = err_count
        
    return client.call('nvme_controllers_error_injection', params)








