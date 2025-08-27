#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2021 Intel Corporation.
#  All rights reserved.

from spdk.rpc.helpers import deprecated_method


@deprecated_method
def ioat_scan_accel_module(client):
    """Enable IOAT accel module.
    """
    return client.call('ioat_scan_accel_module')
