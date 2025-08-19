#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2018 Intel Corporation.
#  All rights reserved.

from spdk.rpc.helpers import deprecated_method


@deprecated_method
def framework_get_subsystems(client):
    return client.call('framework_get_subsystems')


@deprecated_method
def framework_get_config(client, name):
    params = {'name': name}
    return client.call('framework_get_config', params)


@deprecated_method
def framework_get_pci_devices(client):
    return client.call('framework_get_pci_devices')
