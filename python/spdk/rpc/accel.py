from spdk.rpc.helpers import deprecated_alias


def accel_get_opc_assignments(client):
    """Get list of opcode name to module assignments.
    """
    return client.call('accel_get_opc_assignments')


@deprecated_alias('accel_get_engine_info')
def accel_get_module_info(client):
    """Get list of valid module names and their operations.
    """
    return client.call('accel_get_module_info')


def accel_assign_opc(client, opname, module):
    """Manually assign an operation to a module.

    Args:
        opname: name of operation
        module: name of module
    """
    params = {
        'opname': opname,
        'module': module,
    }

    return client.call('accel_assign_opc', params)
