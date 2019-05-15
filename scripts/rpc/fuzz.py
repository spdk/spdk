def fuzz_vhost_create_dev(client, socket, is_blk, use_bogus_buffer, use_valid_buffer, test_scsi_tmf, valid_lun):
    """Create a new device in the vhost fuzzer.

    Args:
        socket: A valid unix domain socket for the dev to bind to.
        is_blk: if set, create a virtio_blk device, otherwise use scsi.
        use_bogus_buffer: if set, pass an invalid memory address as a buffer accompanying requests.
        use_valid_buffer: if set, pass in a valid memory buffer with requests. Overrides use_bogus_buffer.
        test_scsi_tmf: Test scsi management commands on the given device. Valid if and only if is_blk is false.
        valid_lun: Supply only a valid lun number when submitting commands to the given device. Valid if and only if is_blk is false.

    Returns:
        True or False
    """

    params = {"socket": socket,
              "is_blk": is_blk,
              "use_bogus_buffer": use_bogus_buffer,
              "use_valid_buffer": use_valid_buffer,
              "test_scsi_tmf": test_scsi_tmf,
              "valid_lun": valid_lun}

    return client.call("fuzz_vhost_create_dev", params)
