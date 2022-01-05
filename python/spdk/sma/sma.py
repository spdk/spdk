from concurrent import futures
from contextlib import contextmanager
from multiprocessing import Lock
import grpc
import logging
from .device import DeviceException
from .proto import sma_pb2 as pb2
from .proto import sma_pb2_grpc as pb2_grpc


class StorageManagementAgent(pb2_grpc.StorageManagementAgentServicer):
    def __init__(self, addr, port):
        self._devices = {}
        self._server = grpc.server(futures.ThreadPoolExecutor(max_workers=1))
        self._server.add_insecure_port(f'{addr}:{port}')
        pb2_grpc.add_StorageManagementAgentServicer_to_server(self, self._server)

    def _grpc_method(f):
        def wrapper(self, request, context):
            logging.debug(f'{f.__name__}\n{request}')
            return f(self, request, context)
        return wrapper

    def register_device(self, device_manager):
        self._devices[device_manager.name] = device_manager

    def run(self):
        self._server.start()
        self._server.wait_for_termination()

    def _find_device(self, name):
        return self._devices.get(name)

    @_grpc_method
    def CreateDevice(self, request, context):
        response = pb2.CreateDeviceResponse()
        try:
            manager = self._find_device(request.WhichOneof('params'))
            if manager is None:
                raise DeviceException(grpc.StatusCode.INVALID_ARGUMENT,
                                      'Unsupported device type')
            response = manager.create_device(request)
        except DeviceException as ex:
            context.set_details(ex.message)
            context.set_code(ex.code)
        except NotImplementedError:
            context.set_details('Method is not implemented by selected device type')
            context.set_code(grpc.StatusCode.UNIMPLEMENTED)
        return response
