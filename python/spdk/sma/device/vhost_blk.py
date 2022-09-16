import logging
import os
from socket import AddressFamily

import grpc
from spdk.rpc.client import JSONRPCException

from ..common import format_volume_id, volume_id_to_nguid
from ..proto import sma_pb2, virtio_blk_pb2
from ..qmp import QMPClient, QMPError
from ..volume import CryptoException, get_crypto_engine
from .device import DeviceException, DeviceManager


class VhostBlkDeviceManager(DeviceManager):
    def __init__(self, client):
        super().__init__('vhost_blk', 'virtio_blk', client)

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
        self._vhost_path = config.get('sock_path', '/var/tmp/')
        self._prefix = f'{self.protocol}'

    def owns_device(self, id):
        return id.startswith(self._prefix)

    def _find_controller(self, client, controller):
        try:
            ctrlrs = client.call('vhost_get_controllers')
            for ctrlr in ctrlrs:
                if ctrlr['ctrlr'] == controller:
                    return ctrlr
        except JSONRPCException:
            logging.error('Failed to find vhost controller')
        return None

    def _qmp_delete_device(self, ctrlr):
        try:
            with QMPClient(self._qmp_addr, AddressFamily.AF_INET) as qclient:
                if self._find_pcidev(qclient, ctrlr) is not None:
                    qclient.device_del({'id': ctrlr}, {'event': 'DEVICE_DELETED',
                                                       'data': {'device': ctrlr}})
        except QMPError:
            logging.error('QMP: Failed to delete device')
        try:
            with QMPClient(self._qmp_addr, AddressFamily.AF_INET) as qclient:
                if (self._find_pcidev(qclient, ctrlr) is None and
                        self._find_chardev(qclient, ctrlr) is not None):
                    qclient.chardev_remove({'id': ctrlr})
            return True
        except QMPError:
            logging.error('QMP: Failed to delete chardev')
        return False

    def _delete_controller(self, client, ctrlr):
        if self._find_controller(client, ctrlr) is None:
            return True
        try:
            return client.call('vhost_delete_controller', {'ctrlr': ctrlr})
        except JSONRPCException:
            logging.error('Failed to delete controller')
        return False

    def _find_bdev(self, client, guid):
        try:
            bdev_name = get_crypto_engine().get_crypto_bdev(guid) or guid
            return client.call('bdev_get_bdevs', {'name': bdev_name})[0]
        except (JSONRPCException, CryptoException):
            return None

    def _bdev_cmp(self, client, bdev1, bdev2):
        try:
            return self._find_bdev(client, bdev1)['name'] == self._find_bdev(client, bdev2)['name']
        except (KeyError, TypeError):
            return False

    def _create_controller(self, client, ctrlr, volume_guid):
        nctrlr = self._find_controller(client, ctrlr)
        if nctrlr is not None:
            return self._bdev_cmp(client, nctrlr['backend_specific']['block']['bdev'], volume_guid)
        try:
            bdev_name = get_crypto_engine().get_crypto_bdev(volume_guid) or volume_guid
            return client.call('vhost_create_blk_controller',
                               {'ctrlr': ctrlr, 'dev_name': bdev_name})
        except JSONRPCException:
            logging.error('Failed to create subsystem')
        return False

    def _find_pcidev(self, qclient, name):
        try:
            buses = qclient.query_pci()['return']
            for bus in buses:
                for dev in bus['devices']:
                    if 'pci_bridge' in dev:
                        for pcidev in dev['pci_bridge']['devices']:
                            if pcidev['qdev_id'] == name:
                                return pcidev
            return None
        except QMPError:
            return None

    def _find_chardev(self, qclient, name):
        try:
            devs = qclient.query_chardev()['return']
            for dev in devs:
                if dev['label'] == name:
                    return dev
            return None
        except QMPError:
            return None

    def _qmp_add_device(self, ctrlr, phid, sock_path):
        # Find a bus that the physical_id maps to
        for bus in self._buses:
            if phid >= bus.get('count'):
                phid = phid - bus.get('count')
            else:
                break
        else:
            raise DeviceException(grpc.StatusCode.INVALID_ARGUMENT, 'Invalid physical_id')
        try:
            with QMPClient(self._qmp_addr, AddressFamily.AF_INET) as qclient:
                if self._find_chardev(qclient, ctrlr) is None:
                    qclient.chardev_add({
                                        'id': ctrlr,
                                        'backend': {
                                            'type': 'socket',
                                            'data': {
                                                'addr': {
                                                    'type': 'unix',
                                                    'data': {
                                                        'path': os.path.join(sock_path, ctrlr),
                                                    }
                                                },
                                                'server': False,
                                            }
                                        }})
                if self._find_pcidev(qclient, ctrlr) is None:
                    qclient.device_add({'driver': 'vhost-user-blk-pci',
                                        'chardev': ctrlr,
                                        'bus': bus.get('name'),
                                        'addr': hex(phid),
                                        'id': ctrlr})
                return True
        except QMPError:
            self._qmp_delete_device(ctrlr)
            logging.error('QMP: Failed to add device')
        return False

    def create_device(self, request):
        params = request.virtio_blk
        ctrlr = f'sma-{params.physical_id}'
        volume_guid = format_volume_id(request.volume.volume_id)
        if params.virtual_id != 0:
            raise DeviceException(grpc.StatusCode.INVALID_ARGUMENT,
                                  'Unsupported virtual_id value')
        with self._client() as client:
            rc = self._create_controller(client, ctrlr, volume_guid)
            if not rc:
                raise DeviceException(grpc.StatusCode.INTERNAL,
                                      'Failed to create vhost device')
            rc = self._qmp_add_device(ctrlr, params.physical_id, self._vhost_path)
            if not rc:
                self._delete_controller(client, ctrlr)
                raise DeviceException(grpc.StatusCode.INTERNAL,
                                      'Failed to create vhost device')
        return sma_pb2.CreateDeviceResponse(handle=f'{self.protocol}:{ctrlr}')

    def delete_device(self, request):
        with self._client() as client:
            ctrlr = request.handle[len(f'{self._prefix}:'):]
            if not self._qmp_delete_device(ctrlr):
                raise DeviceException(grpc.StatusCode.INTERNAL,
                                      'Failed to delete vhost device')
            if not self._delete_controller(client, ctrlr):
                raise DeviceException(grpc.StatusCode.INTERNAL,
                                      'Failed to delete vhost device')
