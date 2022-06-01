def accel_get_opc_assignments(client):
    """Get list of opcode name to engine assignments.
    """
    return client.call('accel_get_opc_assignments')


def accel_get_engine_info(client):
    """Get list of valid engine names and their operations.
    """
    return client.call('accel_get_engine_names')


def accel_assign_opc(client, opname, engine):
    """Manually assign an operation to an engine.

    Args:
        opname: name of operation
        engine: name of engine
    """
    params = {
        'opname': opname,
        'engine': engine,
    }

    return client.call('accel_assign_opc', params)
