#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2022 Intel Corporation.
#  All rights reserved.

from spdk.rpc.helpers import deprecated_alias


@deprecated_alias('dsa_scan_accel_engine')
def dsa_scan_accel_module(client, config_kernel_mode=None):
    """Scan and enable DSA accel module.

    Args:
        config_kernel_mode: Use kernel DSA driver. (optional)
    """
    params = {}

    if config_kernel_mode is not None:
        params['config_kernel_mode'] = config_kernel_mode
    return client.call('dsa_scan_accel_module', params)
