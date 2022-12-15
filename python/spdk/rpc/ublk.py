#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2022 Intel Corporation.
#  All rights reserved.

def ublk_create_target(client, cpumask=None):
    params = {}
    if cpumask:
        params['cpumask'] = cpumask
    return client.call('ublk_create_target', params)


def ublk_destroy_target(client):
    return client.call('ublk_destroy_target')
