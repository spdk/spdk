#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2022 Intel Corporation.
#  All rights reserved.

from spdk.rpc.helpers import deprecated_alias


@deprecated_alias('iaa_scan_accel_engine')
def iaa_scan_accel_module(client):
    """Scan and enable IAA accel module.
    """
    return client.call('iaa_scan_accel_module')
