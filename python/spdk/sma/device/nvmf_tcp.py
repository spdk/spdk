#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2022 Intel Corporation.
#  All rights reserved.

import grpc
import logging
import uuid
from spdk.rpc.client import JSONRPCException
from spdk.sma import qos
from .device import DeviceManager, DeviceException
from ..common import format_volume_id, volume_id_to_nguid
from ..volume import get_crypto_engine, CryptoException
from ..proto import sma_pb2
from ..proto import nvmf_tcp_pb2


class NvmfTcpDeviceManager(DeviceManager):
    def __init__(self, client):
        super().__init__('nvmf_tcp', 'nvmf_tcp', client)

    def init(self, config):
        self._has_transport = self._create_transport()

    def _create_transport(self):
        try:
            with self._client() as client:
                transports = client.call('nvmf_get_transports')
                for transport in transports:
                    if transport['trtype'].lower() == 'tcp':
                        return True
                # TODO: take the transport params from config
                return client.call('nvmf_create_transport',
                                   {'trtype': 'tcp'})
        except JSONRPCException:
            logging.error('Failed to query for NVMe/TCP transport')
            return False

    def _check_transport(f):
        def wrapper(self, *args):
            if not self._has_transport:
                raise DeviceException(grpc.StatusCode.INTERNAL,
                                      'NVMe/TCP transport is unavailable')
            return f(self, *args)
        return wrapper

    def _get_params(self, request, params):
        result = {}
        for grpc_param, *rpc_param in params:
            rpc_param = rpc_param[0] if rpc_param else grpc_param
            result[rpc_param] = getattr(request, grpc_param)
        return result

    def _check_addr(self, addr, addrlist):
        return next(filter(lambda a: (
            a['trtype'].lower() == 'tcp' and
            a['adrfam'].lower() == addr['adrfam'].lower() and
            a['traddr'].lower() == addr['traddr'].lower() and
            a['trsvcid'].lower() == addr['trsvcid'].lower()), addrlist), None) is not None

    def _get_nqn_from_handle(self, handle):
        return handle[len('nvmf-tcp:'):]

    @_check_transport
    def create_device(self, request):
        params = request.nvmf_tcp
        with self._client() as client:
            try:
                subsystems = client.call('nvmf_get_subsystems')
                for subsystem in subsystems:
                    if subsystem['nqn'] == params.subnqn:
                        break
                else:
                    subsystem = None
                    result = client.call('nvmf_create_subsystem',
                                         {**self._get_params(params, [
                                                ('subnqn', 'nqn'),
                                                ('allow_any_host',)])})
            except JSONRPCException:
                raise DeviceException(grpc.StatusCode.INTERNAL,
                                      'Failed to create NVMe/TCP device')
            try:
                for host in params.hosts:
                    client.call('nvmf_subsystem_add_host',
                                {'nqn': params.subnqn,
                                 'host': host})
                if subsystem is not None:
                    for host in [h['nqn'] for h in subsystem['hosts']]:
                        if host not in params.hosts:
                            client.call('nvmf_subsystem_remove_host',
                                        {'nqn': params.subnqn,
                                         'host': host})

                addr = self._get_params(params, [
                                ('adrfam',),
                                ('traddr',),
                                ('trsvcid',)])
                if subsystem is None or not self._check_addr(addr,
                                                             subsystem['listen_addresses']):
                    client.call('nvmf_subsystem_add_listener',
                                {'nqn': params.subnqn,
                                 'listen_address': {'trtype': 'tcp', **addr}})
                volume_id = format_volume_id(request.volume.volume_id)
                if volume_id is not None:
                    bdev_name = get_crypto_engine().get_crypto_bdev(volume_id) or volume_id
                    result = client.call('nvmf_subsystem_add_ns',
                                         {'nqn': params.subnqn,
                                          'namespace': {
                                              'bdev_name': bdev_name,
                                              'uuid': volume_id,
                                              'nguid': volume_id_to_nguid(volume_id)}})
            except (JSONRPCException, CryptoException):
                try:
                    client.call('nvmf_delete_subsystem', {'nqn': params.subnqn})
                except JSONRPCException:
                    logging.warning(f'Failed to delete subsystem: {params.subnqn}')
                raise DeviceException(grpc.StatusCode.INTERNAL,
                                      'Failed to create NVMe/TCP device')

        return sma_pb2.CreateDeviceResponse(handle=f'nvmf-tcp:{params.subnqn}')

    @_check_transport
    def delete_device(self, request):
        with self._client() as client:
            nqn = self._get_nqn_from_handle(request.handle)
            subsystems = client.call('nvmf_get_subsystems')
            for subsystem in subsystems:
                if subsystem['nqn'] == nqn:
                    result = client.call('nvmf_delete_subsystem',
                                         {'nqn': nqn})
                    if not result:
                        raise DeviceException(grpc.StatusCode.INTERNAL,
                                              'Failed to delete device')
                    break
            else:
                logging.info(f'Tried to delete a non-existing device: {nqn}')

    def _find_bdev(self, client, guid):
        try:
            bdev_name = get_crypto_engine().get_crypto_bdev(guid) or guid
            return client.call('bdev_get_bdevs', {'name': bdev_name})[0]
        except (JSONRPCException, CryptoException):
            return None

    @_check_transport
    def attach_volume(self, request):
        nqn = self._get_nqn_from_handle(request.device_handle)
        volume_id = format_volume_id(request.volume.volume_id)
        if volume_id is None:
            raise DeviceException(grpc.StatusCode.INVALID_ARGUMENT,
                                  'Invalid volume ID')
        try:
            with self._client() as client:
                bdev = self._find_bdev(client, volume_id)
                if bdev is None:
                    raise DeviceException(grpc.StatusCode.NOT_FOUND,
                                          'Invalid volume GUID')
                subsystems = client.call('nvmf_get_subsystems')
                for subsys in subsystems:
                    if subsys['nqn'] == nqn:
                        break
                else:
                    raise DeviceException(grpc.StatusCode.NOT_FOUND,
                                          'Invalid device handle')
                if bdev['name'] not in [ns['name'] for ns in subsys['namespaces']]:
                    result = client.call('nvmf_subsystem_add_ns',
                                         {'nqn': nqn,
                                          'namespace': {
                                              'bdev_name': bdev['name'],
                                              'uuid': volume_id,
                                              'nguid': volume_id_to_nguid(volume_id)}})
                    if not result:
                        raise DeviceException(grpc.StatusCode.INTERNAL,
                                              'Failed to attach volume')
        except JSONRPCException:
            raise DeviceException(grpc.StatusCode.INTERNAL,
                                  'Failed to attach volume')

    @_check_transport
    def detach_volume(self, request):
        nqn = self._get_nqn_from_handle(request.device_handle)
        volume = format_volume_id(request.volume_id)
        if volume is None:
            raise DeviceException(grpc.StatusCode.INVALID_ARGUMENT,
                                  'Invalid volume ID')
        try:
            with self._client() as client:
                bdev = self._find_bdev(client, volume)
                if bdev is None:
                    logging.info(f'Tried to detach non-existing volume: {volume}')
                    return

                subsystems = client.call('nvmf_get_subsystems')
                for subsys in subsystems:
                    if subsys['nqn'] == nqn:
                        break
                else:
                    logging.info(f'Tried to detach volume: {volume} from non-existing ' +
                                 f'device: {nqn}')
                    return

                for ns in subsys['namespaces']:
                    if ns['name'] != bdev['name']:
                        continue
                    result = client.call('nvmf_subsystem_remove_ns',
                                         {'nqn': nqn,
                                          'nsid': ns['nsid']})
                    if not result:
                        raise DeviceException(grpc.StatusCode.INTERNAL,
                                              'Failed to detach volume')
                    break
        except JSONRPCException:
            raise DeviceException(grpc.StatusCode.INTERNAL,
                                  'Failed to detach volume')

    def owns_device(self, handle):
        return handle.startswith('nvmf-tcp')

    def set_qos(self, request):
        nqn = self._get_nqn_from_handle(request.device_handle)
        volume = format_volume_id(request.volume_id)
        if volume is None:
            raise DeviceException(grpc.StatusCode.INVALID_ARGUMENT,
                                  'Invalid volume ID')
        try:
            with self._client() as client:
                # Make sure that a volume exists and is attached to the device
                bdev = self._find_bdev(client, volume)
                if bdev is None:
                    raise DeviceException(grpc.StatusCode.NOT_FOUND,
                                          'No volume associated with volume_id could be found')
                try:
                    subsys = client.call('nvmf_get_subsystems', {'nqn': nqn})[0]
                except JSONRPCException:
                    raise DeviceException(grpc.StatusCode.NOT_FOUND,
                                          'No device associated with device_handle could be found')
                for ns in subsys['namespaces']:
                    if ns['name'] == bdev['name']:
                        break
                else:
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
