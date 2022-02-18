from ..proto import sma_pb2


class DeviceException(Exception):
    def __init__(self, code, message):
        self.code = code
        self.message = message


class DeviceManager:
    def __init__(self, name, protocol, client):
        self._client = client
        self.protocol = protocol
        self.name = name

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
