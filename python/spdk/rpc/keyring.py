# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2024 Intel Corporation.  All rights reserved.


def keyring_file_add_key(client, name, path):
    return client.call('keyring_file_add_key', {'name': name,
                                                'path': path})


def keyring_file_remove_key(client, name):
    return client.call('keyring_file_remove_key', {'name': name})


def keyring_get_keys(client):
    return client.call('keyring_get_keys')


def keyring_linux_set_options(client, enable=None):
    params = {}
    if enable is not None:
        params['enable'] = enable
    return client.call('keyring_linux_set_options', params)
