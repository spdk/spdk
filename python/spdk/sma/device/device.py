#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2022 Intel Corporation.
#  All rights reserved.

from ..proto import sma_pb2


class DeviceException(Exception):
    def __init__(self, code, message):
        self.code = code
        self.message = message


class DeviceManager:
    def __init__(self, name, protocol, client, allow_delete_volumes=False):
        self._client = client
        self.protocol = protocol
        self.name = name
        # Configures whether the device allows deleting a device with attached volumes
        self.allow_delete_volumes = allow_delete_volumes

    def init(self, config):
        pass

    def create_device(self, request):
        raise NotImplementedError()

    def delete_device(self, request):
        raise NotImplementedError()

    def attach_volume(self, request):
        raise NotImplementedError()

    def detach_volume(self, request):
        raise NotImplementedError()

    def owns_device(self, id):
        raise NotImplementedError()

    def set_qos(self, request):
        raise NotImplementedError()

    def get_qos_capabilities(self, request):
        raise NotImplementedError()
