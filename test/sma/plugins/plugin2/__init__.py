from spdk.sma import DeviceManager
from spdk.sma.proto import sma_pb2


class TestDeviceManager1(DeviceManager):
    def __init__(self, client):
        super().__init__('plugin2-device1', 'nvme', client)

    def create_device(self, request):
        return sma_pb2.CreateDeviceResponse(handle=f'{self.protocol}:{self.name}')


class TestDeviceManager2(DeviceManager):
    def __init__(self, client):
        super().__init__('plugin2-device2', 'nvmf_tcp', client)

    def create_device(self, request):
        return sma_pb2.CreateDeviceResponse(handle=f'{self.protocol}:{self.name}')


devices = [TestDeviceManager1, TestDeviceManager2]
