#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) 2022-2024, NVIDIA CORPORATION & AFFILIATES.
#  All rights reserved.

from spdk.rpc.helpers import deprecated_alias


def mlx5_scan_accel_module(client, qp_size=None, num_requests=None, allowed_devs=None,):
    """Enable mlx5 accel module. Scans all mlx5 devices which can perform needed operations

    Args:
        qp_size: Qpair size. (optional)
        num_requests: size of a global requests pool per mlx5 device (optional)
        allowed_devs: Comma separated list of allowed device names, e.g. mlx5_0,mlx5_1
    """
    params = {}

    if qp_size is not None:
        params['qp_size'] = qp_size
    if num_requests is not None:
        params['num_requests'] = num_requests
    if allowed_devs is not None:
        params['allowed_devs'] = allowed_devs
    return client.call('mlx5_scan_accel_module', params)
