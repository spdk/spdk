#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2022 Intel Corporation.
#  All rights reserved.

from spdk.sma import DeviceManager
from spdk.sma import CryptoEngine, get_crypto_engine
from spdk.sma.proto import sma_pb2


class TestCryptoEngine(CryptoEngine):
    def __init__(self):
        super().__init__('crypto-plugin1')

    def setup(self, volume_id, key, cipher, key2=None):
        pass

    def cleanup(self, volume_id):
        pass

    def verify(self, volume_id, key, cipher, key2=None):
        pass

    def get_crypto_bdev(self, volume_id):
        return volume_id


class TestDeviceManager1(DeviceManager):
    def __init__(self, client):
        super().__init__('plugin1-device1', 'nvme', client)

    def create_device(self, request):
        crypto = get_crypto_engine().name
        return sma_pb2.CreateDeviceResponse(handle=f'{self.protocol}:{self.name}:{crypto}')


class TestDeviceManager2(DeviceManager):
    def __init__(self, client):
        super().__init__('plugin1-device2', 'nvmf_tcp', client)

    def create_device(self, request):
        crypto = get_crypto_engine().name
        return sma_pb2.CreateDeviceResponse(handle=f'{self.protocol}:{self.name}:{crypto}')


devices = [TestDeviceManager1, TestDeviceManager2]
crypto_engines = [TestCryptoEngine]
