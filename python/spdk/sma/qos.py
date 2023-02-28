#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2022 Intel Corporation.
#  All rights reserved.

import grpc

from spdk.rpc.client import JSONRPCException
from .common import format_volume_id
from .proto import sma_pb2


LIMIT_UNDEFINED = (1 << 64) - 1


class QosException(Exception):
    def __init__(self, code, message):
        self.code = code
        self.message = message


def set_volume_bdev_qos(client, params):
    class BdevLimit:
        def __init__(self, name, transform=lambda v: v):
            self.name = name
            self._transform = transform

        def get_value(self, value):
            return self._transform(value)

    supported_limits = {
            'rw_iops': BdevLimit('rw_ios_per_sec', lambda v: v * 1000),
            'rd_bandwidth': BdevLimit('r_mbytes_per_sec'),
            'wr_bandwidth': BdevLimit('w_mbytes_per_sec'),
            'rw_bandwidth': BdevLimit('rw_mbytes_per_sec')
    }
    # Check that none of the unsupported fields aren't set either
    if params.HasField('maximum'):
        for field, value in params.maximum.ListFields():
            if field.name in supported_limits.keys():
                continue
            if value != 0 and value != LIMIT_UNDEFINED:
                raise QosException(grpc.StatusCode.INVALID_ARGUMENT,
                                   f'Unsupported QoS limit: maximum.{field.name}')
    try:
        rpc_params = {'name': format_volume_id(params.volume_id)}
        for name, limit in supported_limits.items():
            value = getattr(params.maximum, name)
            if value != LIMIT_UNDEFINED:
                rpc_params[limit.name] = limit.get_value(value)
        client.call('bdev_set_qos_limit', rpc_params)
    except JSONRPCException:
        raise QosException(grpc.StatusCode.INTERNAL, 'Failed to set QoS')


def get_bdev_qos_capabilities():
    return sma_pb2.GetQosCapabilitiesResponse(
            max_volume_caps=sma_pb2.GetQosCapabilitiesResponse.QosCapabilities(
                rw_iops=True,
                rw_bandwidth=True,
                rd_bandwidth=True,
                wr_bandwidth=True
                ),
            )
