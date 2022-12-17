#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2019 Intel Corporation.
#  All rights reserved.


def env_dpdk_get_mem_stats(client):
    """Dump the applications memory stats to a file.

    Returns:
        The path to the file where the stats are written.
    """

    return client.call('env_dpdk_get_mem_stats')
