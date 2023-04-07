#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2022 Intel Corporation.
#  All rights reserved.
#

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


def accel_crypto_key_create(client, cipher, key, key2, tweak_mode, name):
    """Create Data Encryption Key Identifier.

    Args:
        cipher: cipher
        key: key
        key2: key2
        tweak_mode: tweak mode
        name: key name
    """
    params = {
        'cipher': cipher,
        'key': key,
        'name': name,
    }
    if key2 is not None:
        params['key2'] = key2
    if tweak_mode is not None:
        params['tweak_mode'] = tweak_mode

    return client.call('accel_crypto_key_create', params)


def accel_crypto_key_destroy(client, key_name):
    """Destroy Data Encryption Key.

    Args:
        key_name: key name
    """
    params = {
        'key_name': key_name
    }

    return client.call('accel_crypto_key_destroy', params)


def accel_crypto_keys_get(client, key_name):
    """Get a list of the crypto keys.

    Args:
        key_name: Get information about a specific key
    """
    params = {}

    if key_name is not None:
        params['key_name'] = key_name

    return client.call('accel_crypto_keys_get', params)


def accel_set_driver(client, name):
    """Select accel platform driver to execute operation chains.

    Args:
        name: name of the driver
    """
    return client.call('accel_set_driver', {'name': name})


def accel_set_options(client, small_cache_size, large_cache_size,
                      task_count, sequence_count, buf_count):
    """Set accel framework's options."""
    params = {}

    if small_cache_size is not None:
        params['small_cache_size'] = small_cache_size
    if large_cache_size is not None:
        params['large_cache_size'] = large_cache_size
    if task_count is not None:
        params['task_count'] = task_count
    if sequence_count is not None:
        params['sequence_count'] = sequence_count
    if buf_count is not None:
        params['buf_count'] = buf_count

    return client.call('accel_set_options', params)


def accel_get_stats(client):
    """Get accel framework's statistics"""

    return client.call('accel_get_stats')
