

def nvme_cmd(client, name, type, data_direction, cmdbuf,
             data=None, metadata=None,
             data_len=None, metadata_len=None,
             timeout_ms=0):
    """Raise one NVMe command

    Args:
        name: Name of the NVMe device
        type: Type of nvme cmd. Valid values are: admin, io
        data-direction: Direction of data transfer. Valid values are: c2h, h2c
        cmdbuf: NVMe command encoded by base64 urlsafe
        data: Data transferring to controller from host, encoded by base64 urlsafe
        metadata: metadata transferring to controller from host, encoded by base64 urlsafe
        data-length: Data length required to transfer from controller to host
        metadata-length: Metadata length required to transfer from controller to host
        timeout-ms: Command execution timeout value, in milliseconds, if 0, don't track timeout

    Returns:
        NVMe completion queue entry, requested data and metadata.
    """
    params = {'name': name,
              'type': type,
              'data_direction': data_direction,
              'cmdbuf': cmdbuf}

    if data:
        params['data'] = data

    if metadata:
        params['metadata'] = metadata

    if data_len:
        data_len['data_len'] = data_len

    if metadata_len:
        params['metadata_len'] = metadata_len

    if timeout_ms:
        params['timeout_ms'] = timeout_ms

    return client.call('nvme_cmd', params)
