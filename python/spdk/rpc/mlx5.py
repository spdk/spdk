#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) 2022-2024, NVIDIA CORPORATION & AFFILIATES.
#  All rights reserved.

from spdk.rpc.helpers import deprecated_alias


def mlx5_scan_accel_module(client, qp_size=None, num_requests=None, allowed_devs=None, crypto_split_blocks=None):
    """Enable mlx5 accel module. Scans all mlx5 devices which can perform needed operations

    Args:
        qp_size: Qpair size. (optional)
        num_requests: size of a global requests pool per mlx5 device (optional)
        allowed_devs: Comma separated list of allowed device names, e.g. mlx5_0,mlx5_1
        crypto_split_blocks: Number of data blocks to be processed in 1 crypto UMR. [0-65535], 0 means no limit
    """
    params = {}

    if qp_size is not None:
        params['qp_size'] = qp_size
    if num_requests is not None:
        params['num_requests'] = num_requests
    if allowed_devs is not None:
        params['allowed_devs'] = allowed_devs
    if crypto_split_blocks is not None:
        params['crypto_split_blocks'] = crypto_split_blocks
    return client.call('mlx5_scan_accel_module', params)


def accel_mlx5_dump_stats(client, level=None):

    params = {}

    if level is not None:
        params['level'] = level
    return client.call('accel_mlx5_dump_stats', params)
