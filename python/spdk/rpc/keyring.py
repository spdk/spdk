# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2024 Intel Corporation.  All rights reserved.

from spdk.rpc.helpers import deprecated_method


@deprecated_method
def keyring_file_add_key(client, name, path):
    return client.call('keyring_file_add_key', {'name': name,
                                                'path': path})


@deprecated_method
def keyring_file_remove_key(client, name):
    return client.call('keyring_file_remove_key', {'name': name})


@deprecated_method
def keyring_get_keys(client):
    return client.call('keyring_get_keys')


@deprecated_method
def keyring_linux_set_options(client, enable=None):
    params = {}
    if enable is not None:
        params['enable'] = enable
    return client.call('keyring_linux_set_options', params)
