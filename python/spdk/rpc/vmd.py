#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2019 Intel Corporation.
#  All rights reserved.

from .helpers import deprecated_alias


@deprecated_alias('enable_vmd')
def vmd_enable(client):
    """Enable VMD enumeration."""
    return client.call('vmd_enable')


def vmd_remove_device(client, addr):
    """Remove a device behind VMD"""
    return client.call('vmd_remove_device', {'addr': addr})


def vmd_rescan(client):
    """Force a rescan of the devices behind VMD"""
    return client.call('vmd_rescan')
