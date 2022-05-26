def accel_get_opc_assignments(client):
    """Get list of opcode name to engine assignments.
    """
    return client.call('accel_get_opc_assignments')
