#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2022 Intel Corporation.
#  All rights reserved.

from concurrent import futures
from contextlib import contextmanager
from multiprocessing import Lock
import grpc
import logging
from .device import DeviceException
from .volume import VolumeException, VolumeManager
from .volume import crypto
from .volume import crypto_bdev
from .proto import sma_pb2 as pb2
from .proto import sma_pb2_grpc as pb2_grpc


class StorageManagementAgent(pb2_grpc.StorageManagementAgentServicer):
    def __init__(self, config, client):
        addr, port = config['address'], config['port']
        self._devices = {}
        self._server = grpc.server(futures.ThreadPoolExecutor(max_workers=1))
        self._server.add_insecure_port(f'{addr}:{port}')
        self._volume_mgr = VolumeManager(client, config['discovery_timeout'],
                                         config['volume_cleanup_period'])
        pb2_grpc.add_StorageManagementAgentServicer_to_server(self, self._server)

    def _grpc_method(f):
        def wrapper(self, request, context):
            logging.debug(f'{f.__name__}\n{request}')
            return f(self, request, context)
        return wrapper

    def register_device(self, device_manager):
        self._devices[device_manager.protocol] = device_manager

    def start(self):
        self._volume_mgr.start()
        self._server.start()

    def stop(self):
        self._server.stop(None)
        self._volume_mgr.stop()

    def _find_device_by_name(self, name):
        return self._devices.get(name)

    def _find_device_by_handle(self, handle):
        for device in self._devices.values():
            try:
                if device.owns_device(handle):
                    return device
            except NotImplementedError:
                pass
        return None

    def _cleanup_volume(self, volume_id, existing):
        if volume_id is None or existing:
            return
        try:
            self._volume_mgr.disconnect_volume(volume_id)
        except VolumeException:
            logging.warning('Failed to cleanup volume {volume_id}')

    @_grpc_method
    def CreateDevice(self, request, context):
        response = pb2.CreateDeviceResponse()
        volume_id, existing = None, False
        try:
            if request.HasField('volume'):
                volume_id, existing = self._volume_mgr.connect_volume(request.volume)

            manager = self._find_device_by_name(request.WhichOneof('params'))
            if manager is None:
                raise DeviceException(grpc.StatusCode.INVALID_ARGUMENT,
                                      'Unsupported device type')
            response = manager.create_device(request)
            # Now that we know the device handle, mark the volume as attached to
            # that device
            if volume_id is not None:
                self._volume_mgr.set_device(volume_id, response.handle)
        except (DeviceException, VolumeException) as ex:
            self._cleanup_volume(volume_id, existing)
            context.set_details(ex.message)
            context.set_code(ex.code)
        except NotImplementedError:
            self._cleanup_volume(volume_id, existing)
            context.set_details('Method is not implemented by selected device type')
            context.set_code(grpc.StatusCode.UNIMPLEMENTED)
        return response

    @_grpc_method
    def DeleteDevice(self, request, context):
        response = pb2.DeleteDeviceResponse()
        try:
            device = self._find_device_by_handle(request.handle)
            if device is None:
                raise DeviceException(grpc.StatusCode.NOT_FOUND,
                                      'Invalid device handle')
            if not device.allow_delete_volumes and self._volume_mgr.has_volumes(request.handle):
                raise DeviceException(grpc.StatusCode.FAILED_PRECONDITION,
                                      'Device has attached volumes')
            device.delete_device(request)
            # Either there are no volumes attached to this device or we're allowed to delete it
            # with volumes still attached
            self._volume_mgr.disconnect_device_volumes(request.handle)
        except DeviceException as ex:
            context.set_details(ex.message)
            context.set_code(ex.code)
        except NotImplementedError:
            context.set_details('Method is not implemented by selected device type')
            context.set_code(grpc.StatusCode.UNIMPLEMENTED)
        return response

    @_grpc_method
    def AttachVolume(self, request, context):
        response = pb2.AttachVolumeResponse()
        volume_id, existing = None, False
        try:
            if not request.HasField('volume'):
                raise VolumeException(grpc.StatusCode.INVALID_ARGUMENT,
                                      'Missing required field: volume')
            volume_id, existing = self._volume_mgr.connect_volume(request.volume,
                                                                  request.device_handle)
            device = self._find_device_by_handle(request.device_handle)
            if device is None:
                raise DeviceException(grpc.StatusCode.NOT_FOUND, 'Invalid device handle')
            device.attach_volume(request)
        except (DeviceException, VolumeException) as ex:
            self._cleanup_volume(volume_id, existing)
            context.set_details(ex.message)
            context.set_code(ex.code)
        except NotImplementedError:
            self._cleanup_volume(volume_id, existing)
            context.set_details('Method is not implemented by selected device type')
            context.set_code(grpc.StatusCode.UNIMPLEMENTED)
        return response

    @_grpc_method
    def DetachVolume(self, request, context):
        response = pb2.DetachVolumeResponse()
        try:
            device = self._find_device_by_handle(request.device_handle)
            if device is not None:
                device.detach_volume(request)
                self._volume_mgr.disconnect_volume(request.volume_id)
        except DeviceException as ex:
            context.set_details(ex.message)
            context.set_code(ex.code)
        return response

    @_grpc_method
    def SetQos(self, request, context):
        response = pb2.SetQosResponse()
        try:
            device = self._find_device_by_handle(request.device_handle)
            if device is None:
                raise DeviceException(grpc.StatusCode.NOT_FOUND, 'Invalid device handle')
            device.set_qos(request)
        except DeviceException as ex:
            context.set_details(ex.message)
            context.set_code(ex.code)
        except NotImplementedError:
            context.set_details('Method is not implemented by selected device type')
            context.set_code(grpc.StatusCode.UNIMPLEMENTED)
        return response

    @_grpc_method
    def GetQosCapabilities(self, request, context):
        device_type_map = {
            pb2.DeviceType.DEVICE_TYPE_NVME: 'nvme',
            pb2.DeviceType.DEVICE_TYPE_VIRTIO_BLK: 'virtio_blk',
            pb2.DeviceType.DEVICE_TYPE_NVMF_TCP: 'nvmf_tcp',
        }
        response = pb2.GetQosCapabilitiesResponse()
        try:
            name = device_type_map.get(request.device_type)
            if name is None:
                raise DeviceException(grpc.StatusCode.INVALID_ARGUMENT,
                                      'Invalid device type')
            manager = self._find_device_by_name(name)
            if manager is None:
                raise DeviceException(grpc.StatusCode.INVALID_ARGUMENT,
                                      'Unsupported device type')
            response = manager.get_qos_capabilities(request)
        except DeviceException as ex:
            context.set_details(ex.message)
            context.set_code(ex.code)
        except NotImplementedError:
            # If a device manager doesn't implement this method, return empty capabilities to
            # indicate that no QoS capabilities are supported
            pass
        return response


crypto.register_crypto_engine(crypto.CryptoEngineNop())
crypto.register_crypto_engine(crypto_bdev.CryptoEngineBdev())
crypto.set_crypto_engine('nop')
