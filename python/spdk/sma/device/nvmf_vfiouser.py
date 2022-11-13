#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2022 Intel Corporation.
#  All rights reserved.

import logging
import os
import shutil
from contextlib import contextmanager
from socket import AddressFamily

import grpc
from google.protobuf import wrappers_pb2 as wrap
from spdk.rpc.client import JSONRPCException
from spdk.sma import qos

from ..common import format_volume_id, volume_id_to_nguid
from ..proto import sma_pb2
from ..qmp import QMPClient, QMPError
from ..volume import CryptoException, get_crypto_engine
from .device import DeviceException, DeviceManager

log = logging.getLogger(__name__)


class NvmfVfioDeviceManager(DeviceManager):
    def __init__(self, client):
        super().__init__('vfiouser', 'nvme', client)

    def init(self, config):
        self._buses = config.get('buses', [])
        try:
            if len(self._buses) != len(list({v['name']: v for v in self._buses}.values())):
                raise DeviceException(grpc.StatusCode.INVALID_ARGUMENT,
                                      'Duplicate PCI bridge names')
        except KeyError:
            raise DeviceException(grpc.StatusCode.INVALID_ARGUMENT,
                                  'PCI bridge name is missing')
        for bus in self._buses:
            bus['count'] = bus.get('count', 32)
            if bus['count'] < 0:
                raise DeviceException(grpc.StatusCode.INVALID_ARGUMENT,
                                      'Incorrect PCI bridge count')
        self._qmp_addr = (config.get('qmp_addr', '127.0.0.1'), config.get('qmp_port'))
        self._sock_path = config.get('sock_path', '/var/tmp/')
        self._prefix = f'{self.protocol}'
        if not self._create_transport():
            raise DeviceException(grpc.StatusCode.INTERNAL,
                                  'NVMe/vfiouser transport is unavailable')

    def _create_transport(self):
        try:
            with self._client() as client:
                transports = client.call('nvmf_get_transports')
                for transport in transports:
                    if transport['trtype'].lower() == 'vfiouser':
                        return True
                return client.call('nvmf_create_transport', {'trtype': 'vfiouser'})
        except JSONRPCException:
            logging.error(f'Transport query NVMe/vfiouser failed')
            return False

    @contextmanager
    def _client_wrap(self):
        try:
            with self._client() as client:
                yield client
        except JSONRPCException:
            raise DeviceException(grpc.StatusCode.INTERNAL,
                                  'Failed to connect to SPDK application')

    def _get_subsys(self, client, nqn):
        try:
            return client.call('nvmf_get_subsystems', {'nqn': nqn})[0]
        except JSONRPCException:
            return False

    def _create_subsystem(self, client, subnqn):
        try:
            if self._get_subsys(client, subnqn):
                return True
            return client.call('nvmf_create_subsystem', {'nqn': subnqn, 'allow_any_host': True})
        except JSONRPCException:
            logging.error('Failed to create subsystem')
        return False

    def _delete_subsystem(self, client, subnqn):
        try:
            if not self._get_subsys(client, subnqn):
                return True
            return client.call('nvmf_delete_subsystem', {'nqn': subnqn})
        except JSONRPCException:
            logging.error('Failed to delete subsystem')
        return False

    def _subsystem_add_listener(self, client, subnqn, addr):
        try:
            return client.call('nvmf_subsystem_add_listener',
                               {'nqn': subnqn,
                                'listen_address': {
                                    'trtype': 'vfiouser',
                                    'traddr': addr}})
        except JSONRPCException:
            logging.error('Failed to add listener')
        return False

    def _create_socket_dir(self, dev_id):
        try:
            path = os.path.join(self._sock_path, dev_id)
            if os.path.exists(path):
                shutil.rmtree(path)
            os.makedirs(path)
            if os.path.isdir(path):
                return path
        except OSError:
            raise DeviceException(grpc.StatusCode.INTERNAL, f'Socket path error {path}')

    def _find_pcidev(self, qclient, name):
        def rsearch(devices, name):
            found_dev = None
            for dev in devices:
                if dev['qdev_id'] == name:
                    found_dev = dev
                elif 'pci_bridge' in dev:
                    found_dev = rsearch(dev['pci_bridge']['devices'], name)

                if found_dev:
                    break
            return found_dev

        try:
            buses = qclient.query_pci()['return']
            for bus in buses:
                rc = rsearch(bus['devices'], name)
                if rc is not None:
                    return rc
        except QMPError:
            return None

    def _qmp_add_device(self, phid, dev_id):
        # Find a bus that the physical_id maps to
        for bus in self._buses:
            if phid >= bus['count']:
                phid = phid - bus['count']
            else:
                break
        else:
            raise DeviceException(grpc.StatusCode.INVALID_ARGUMENT, 'Invalid physical_id')
        try:
            with QMPClient(self._qmp_addr, AddressFamily.AF_INET) as qclient:
                if self._find_pcidev(qclient, dev_id) is None:
                    qclient.device_add({'driver': 'vfio-user-pci',
                                        'x-enable-migration': 'on',
                                        'socket': os.path.join(self._sock_path, dev_id, 'cntrl'),
                                        'bus': bus.get('name'),
                                        'addr': hex(phid),
                                        'id': dev_id})
            return True
        except QMPError:
            logging.error('QMP: Failed to add device')
        return False

    def _create_device(self, physical_id):
        with self._client_wrap() as client:
            dev_id = f'{self.name}-{physical_id}'
            subnqn = f'nqn.2016-06.io.spdk:{dev_id}'
            rc = self._create_subsystem(client, subnqn)
            if not rc:
                raise DeviceException(grpc.StatusCode.INTERNAL,
                                      'Failed to create the NVMe/vfiouser subsystem')
            rc = self._subsystem_add_listener(client, subnqn,
                                              self._create_socket_dir(dev_id))
            if not rc:
                self._delete_subsystem(client, subnqn)
                raise DeviceException(grpc.StatusCode.INTERNAL,
                                      'Failed to add the NVMe/vfiouser listener')
            rc = self._qmp_add_device(physical_id, dev_id)
            if not rc:
                self._delete_subsystem(client, subnqn)
                raise DeviceException(grpc.StatusCode.INTERNAL,
                                      'Failed to create NVMe/vfiouser device')
            return subnqn

    def create_device(self, request):
        if request.nvme.virtual_id != 0:
            raise DeviceException(grpc.StatusCode.INVALID_ARGUMENT,
                                  'Unsupported virtual_id value')
        subnqn = self._create_device(request.nvme.physical_id)
        return sma_pb2.CreateDeviceResponse(handle=f'{self._prefix}:{subnqn}')

    def _qmp_delete_device(self, dev_id):
        try:
            with QMPClient(self._qmp_addr, AddressFamily.AF_INET) as qclient:
                if self._find_pcidev(qclient, dev_id) is not None:
                    qclient.device_del({'id': dev_id})
            return True
        except QMPError:
            logging.error('QMP: Failed to delete device')
        return False

    def delete_device(self, request):
        with self._client_wrap() as client:
            nqn = request.handle[len(f'{self._prefix}:'):]
            dev_id = nqn[len('nqn.2016-06.io.spdk:'):]
            if not self._delete_subsystem(client, nqn):
                raise DeviceException(grpc.StatusCode.INTERNAL,
                                      'Failed to delete NVMe/vfiouser device')
            if not self._qmp_delete_device(dev_id):
                raise DeviceException(grpc.StatusCode.INTERNAL,
                                      'Failed to delete NVMe/vfiouser device')
        try:
            path = os.path.join(self._sock_path, dev_id)
            if os.path.exists(path):
                shutil.rmtree(path)
        except OSError:
            raise DeviceException(grpc.StatusCode.INTERNAL, f'Socket path error {path}')

    def _get_bdev(self, client, guid):
        try:
            bdev_name = get_crypto_engine().get_crypto_bdev(guid) or guid
            return client.call('bdev_get_bdevs', {'name': bdev_name})[0]
        except (JSONRPCException, CryptoException):
            logging.error('Failed to find bdev')
            return None

    def _get_ns(self, bdev, subsystem):
        for ns in subsystem['namespaces']:
            if ns['name'] == bdev['name']:
                return ns

    def _subsystem_add_ns(self, client, bdev, subsystem, subnqn, volume_id):
        try:
            if self._get_ns(bdev, subsystem) is not None:
                return True
            return client.call('nvmf_subsystem_add_ns',
                               {'nqn': subnqn,
                                'namespace': {
                                    'bdev_name': bdev['name'],
                                    'uuid': volume_id,
                                    'nguid': volume_id_to_nguid(volume_id)}})
        except JSONRPCException:
            logging.error('Failed to add ns')
        return False

    def attach_volume(self, request):
        nqn = request.device_handle[len(f'{self._prefix}:'):]
        volume_id = format_volume_id(request.volume.volume_id)
        with self._client_wrap() as client:
            bdev = self._get_bdev(client, volume_id)
            if bdev is None:
                raise DeviceException(grpc.StatusCode.NOT_FOUND,
                                      'Invalid volume GUID')
            subsys = self._get_subsys(client, nqn)
            if subsys is None:
                raise DeviceException(grpc.StatusCode.NOT_FOUND,
                                      'Invalid device handle')
            result = self._subsystem_add_ns(client, bdev, subsys, nqn, volume_id)
            if not result:
                raise DeviceException(grpc.StatusCode.INTERNAL,
                                      'Failed to attach volume')

    def _subsystem_remove_ns(self, client, bdev, subsystem, subnqn):
        try:
            ns = self._get_ns(bdev, subsystem)
            if ns is None:
                return True
            return client.call('nvmf_subsystem_remove_ns',
                               {'nqn': subnqn, 'nsid': ns['nsid']})
        except JSONRPCException:
            logging.error('Failed to remove ns')
        return False

    def detach_volume(self, request):
        nqn = request.device_handle[len(f'{self._prefix}:'):]
        volume_id = format_volume_id(request.volume_id)
        with self._client_wrap() as client:
            bdev = self._get_bdev(client, volume_id)
            if bdev is None:
                raise DeviceException(grpc.StatusCode.NOT_FOUND,
                                      'Invalid volume GUID')
            subsys = self._get_subsys(client, nqn)
            if subsys is None:
                raise DeviceException(grpc.StatusCode.NOT_FOUND,
                                      'Invalid device handle')
            result = self._subsystem_remove_ns(client, bdev, subsys, nqn)
            if not result:
                raise DeviceException(grpc.StatusCode.INTERNAL,
                                      'Failed to detach volume')

    def owns_device(self, id):
        return id.startswith(self._prefix)

    def set_qos(self, request):
        nqn = request.device_handle[len(f'{self._prefix}:'):]
        volume = format_volume_id(request.volume_id)
        if volume is None:
            raise DeviceException(grpc.StatusCode.INVALID_ARGUMENT,
                                  'Invalid volume ID')
        try:
            with self._client() as client:
                # Make sure that a volume exists and is attached to the device
                bdev = self._get_bdev(client, volume)
                if bdev is None:
                    raise DeviceException(grpc.StatusCode.NOT_FOUND,
                                          'No volume associated with volume_id could be found')
                subsys = self._get_subsys(client, nqn)
                if subsys is None:
                    raise DeviceException(grpc.StatusCode.NOT_FOUND,
                                          'No device associated with device_handle could be found')
                ns = self._get_ns(bdev, subsys)
                if ns is None:
                    raise DeviceException(grpc.StatusCode.INVALID_ARGUMENT,
                                          'Specified volume is not attached to the device')
                qos.set_volume_bdev_qos(client, request)
        except qos.QosException as ex:
            raise DeviceException(ex.code, ex.message)
        except JSONRPCException:
            raise DeviceException(grpc.StatusCode.INTERNAL,
                                  'Failed to set QoS')

    def get_qos_capabilities(self, request):
        return qos.get_bdev_qos_capabilities()
