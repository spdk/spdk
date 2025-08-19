#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2019 Intel Corporation.
#  All rights reserved.

from spdk.rpc.helpers import deprecated_alias, deprecated_method


@deprecated_alias('enable_vmd')
@deprecated_method
def vmd_enable(client):
    """Enable VMD enumeration."""
    return client.call('vmd_enable')


@deprecated_method
def vmd_remove_device(client, addr):
    """Remove a device behind VMD"""
    return client.call('vmd_remove_device', {'addr': addr})


@deprecated_method
def vmd_rescan(client):
    """Force a rescan of the devices behind VMD"""
    return client.call('vmd_rescan')
