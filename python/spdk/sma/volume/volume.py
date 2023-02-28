#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2022 Intel Corporation.
#  All rights reserved.

import grpc
import ipaddress
import logging
import threading
import uuid
from dataclasses import dataclass
from spdk.rpc.client import JSONRPCException
from . import crypto
from ..common import format_volume_id
from ..proto import sma_pb2


log = logging.getLogger(__name__)


class VolumeException(Exception):
    def __init__(self, code, message):
        self.code = code
        self.message = message


class Volume:
    def __init__(self, volume_id, device_handle, discovery_services):
        self.volume_id = volume_id
        self.discovery_services = discovery_services
        self.device_handle = device_handle


class VolumeManager:
    def __init__(self, client, discovery_timeout, cleanup_period):
        self._client = client
        # Discovery service map (name -> refcnt)
        self._discovery = {}
        # Volume map (volume_id -> Volume)
        self._volumes = {}
        self._discovery_timeout = int(discovery_timeout * 1000)
        self._cleanup_period = cleanup_period
        self._lock = threading.Lock()
        self._cv = threading.Condition(self._lock)
        self._running = False
        self._thread = None

    def _locked(f):
        def wrapper(self, *args, **kwargs):
            self._lock.acquire()
            try:
                return f(self, *args, **kwargs)
            finally:
                self._lock.release()
        return wrapper

    def start(self):
        if self._thread is not None:
            raise ValueError('Volume manager was already started')
        self._running = True
        self._thread = threading.Thread(target=self._cleanup_thread, args=(self,))
        self._thread.start()

    def stop(self):
        if self._thread is None:
            return
        with self._lock:
            self._running = False
            self._cv.notify_all()
        self._thread.join()
        self._thread = None

    @staticmethod
    def _cleanup_thread(*args):
        self, = args
        with self._lock:
            while self._running:
                self._cleanup_volumes()
                self._cv.wait(self._cleanup_period)

    def _cleanup_volumes(self):
        try:
            disconnected = []
            with self._client() as client:
                bdevs = client.call('bdev_get_bdevs')
                for volume_id in self._volumes:
                    if volume_id not in [b['uuid'] for b in bdevs]:
                        log.warning(f'Found disconnected volume: {volume_id}')
                        disconnected.append(volume_id)
            for volume_id in disconnected:
                self._disconnect_volume(volume_id)
        except VolumeException as ex:
            log.error(f'Failure when trying to disconnect volumes: {ex.message}')
        except JSONRPCException as ex:
            log.error(f'Failed to retrieve bdevs: {ex.message}')

    def _get_discovery_info(self):
        try:
            with self._client() as client:
                return client.call('bdev_nvme_get_discovery_info')
        except JSONRPCException:
            raise VolumeException(grpc.StatusCode.INTERNAL,
                                  'Failed to retrieve discovery service status')

    def _compare_trid(self, trid1, trid2):
        return (trid1['trtype'].lower() == trid2['trtype'].lower() and
                trid1['traddr'].lower() == trid2['traddr'].lower() and
                trid1['trsvcid'].lower() == trid2['trsvcid'].lower() and
                trid1['adrfam'].lower() == trid2['adrfam'].lower())

    def _get_adrfam(self, traddr):
        try:
            return 'ipv{}'.format(ipaddress.ip_address(traddr).version)
        except ValueError:
            raise VolumeException(grpc.StatusCode.INVALID_ARGUMENT,
                                  'Invalid traddr')

    def _get_volume_bdev(self, volume_id, timeout):
        try:
            with self._client() as client:
                return client.call('bdev_get_bdevs',
                                   {'name': volume_id,
                                    'timeout': timeout})[0]
        except JSONRPCException:
            return None

    def _start_discovery(self, trid, hostnqn):
        try:
            # Use random UUID as name
            name = str(uuid.uuid4())
            log.debug(f'Starting discovery service {name}')
            with self._client() as client:
                client.call('bdev_nvme_start_discovery',
                            {'name': name,
                             'wait_for_attach': True,
                             'attach_timeout_ms': self._discovery_timeout,
                             'hostnqn': hostnqn,
                             **trid})
            self._discovery[name] = 1
            return name
        except JSONRPCException:
            raise VolumeException(grpc.StatusCode.INTERNAL,
                                  'Failed to start discovery')

    def _stop_discovery(self, name):
        refcnt = self._discovery.get(name)
        log.debug(f'Stopping discovery service {name}, refcnt={refcnt}')
        if refcnt is None:
            # Should never happen
            log.warning('Tried to stop discovery using non-existing name')
            return
        # Check the refcount to leave the service running if there are more volumes using it
        if refcnt > 1:
            self._discovery[name] = refcnt - 1
            return
        del self._discovery[name]
        try:
            with self._client() as client:
                client.call('bdev_nvme_stop_discovery',
                            {'name': name})
            log.debug(f'Stopped discovery service {name}')
        except JSONRPCException:
            raise VolumeException(grpc.StatusCode.INTERNAL,
                                  'Failed to stop discovery')

    def _get_crypto_params(self, params):
        key, cipher, key2, tweak_mode = None, None, None, None
        try:
            if params.HasField('crypto'):
                key, cipher = params.crypto.key.decode('ascii'), params.crypto.cipher
                if len(params.crypto.key2) > 0:
                    key2 = params.crypto.key2.decode('ascii')
                if params.crypto.tweak_mode is not None:
                    tweak_mode = params.crypto.tweak_mode
        except UnicodeDecodeError:
            raise VolumeException(grpc.StatusCode.INVALID_ARGUMENT,
                                  'Corrupted crypto key')
        return key, cipher, key2, tweak_mode

    def _setup_crypto(self, volume_id, params):
        try:
            if not params.HasField('crypto'):
                return
            key, cipher, key2, tweak_mode = self._get_crypto_params(params)
            crypto.get_crypto_engine().setup(volume_id, key, cipher, key2, tweak_mode)
        except crypto.CryptoException as ex:
            raise VolumeException(ex.code, ex.message)

    def _cleanup_crypto(self, volume_id):
        try:
            crypto.get_crypto_engine().cleanup(volume_id)
        except crypto.CryptoException as ex:
            logging.warning(f'Failed to cleanup crypto: {ex.message}')

    def _verify_crypto(self, volume_id, params):
        try:
            key, cipher, key2, tweak_mode = self._get_crypto_params(params)
            crypto.get_crypto_engine().verify(volume_id, key, cipher, key2, tweak_mode)
        except crypto.CryptoException as ex:
            raise VolumeException(ex.code, ex.message)

    @_locked
    def connect_volume(self, params, device_handle=None):
        """ Connects a volume through a discovery service.  Returns a tuple (volume_id, existing):
        the first item is a volume_id as str, while the second denotes whether the selected volume
        existed prior to calling this method.
        """
        volume_id = format_volume_id(params.volume_id)
        if volume_id is None:
            raise VolumeException(grpc.StatusCode.INVALID_ARGUMENT,
                                  'Invalid volume ID')
        if volume_id in self._volumes:
            volume = self._volumes[volume_id]
            if device_handle is not None and volume.device_handle != device_handle:
                raise VolumeException(grpc.StatusCode.ALREADY_EXISTS,
                                      'Volume is already attached to a different device')
            # Make sure the crypto params are the same
            self._verify_crypto(volume_id, params)
            return volume_id, True
        discovery_services = set()
        try:
            # First start discovery connecting to specified endpoints
            for req_ep in params.nvmf.discovery.discovery_endpoints:
                info = self._get_discovery_info()
                trid = {'trtype': req_ep.trtype,
                        'traddr': req_ep.traddr,
                        'trsvcid': req_ep.trsvcid,
                        'adrfam': self._get_adrfam(req_ep.traddr)}
                name = None
                for discovery in info:
                    if self._compare_trid(discovery['trid'], trid):
                        name = discovery['name']
                        break
                    if next(filter(lambda r: self._compare_trid(r['trid'], trid),
                                   discovery['referrals']), None):
                        name = discovery['name']
                        break
                if name is not None:
                    # If we've already attached a discovery service, it probably means that the user
                    # specified a referred address
                    if name not in discovery_services:
                        refcnt = self._discovery.get(name)
                        if refcnt is None:
                            log.warning('Found a discovery service missing from internal map')
                            refcnt = 0
                        self._discovery[name] = refcnt + 1
                else:
                    name = self._start_discovery(trid, params.nvmf.hostnqn)
                discovery_services.add(name)

            # Now check if a bdev with specified volume_id exists, give it 1s to appear
            bdev = self._get_volume_bdev(volume_id, timeout=1000)
            if bdev is None:
                raise VolumeException(grpc.StatusCode.NOT_FOUND,
                                      'Volume could not be found')
            # Check subsystem's NQN if it's specified
            if params.nvmf.subnqn:
                nvme = bdev.get('driver_specific', {}).get('nvme', [])
                # The NVMe bdev can report multiple subnqns, but they all should be the same, so
                # don't bother checking more than the first one
                subnqn = next(iter(nvme), {}).get('trid', {}).get('subnqn')
                if subnqn != params.nvmf.subnqn:
                    raise VolumeException(grpc.StatusCode.INVALID_ARGUMENT,
                                          'Unexpected subsystem NQN')
            self._setup_crypto(volume_id, params)
            # Finally remember that volume
            self._volumes[volume_id] = Volume(volume_id, device_handle, discovery_services)
        except Exception as ex:
            for name in discovery_services:
                try:
                    self._stop_discovery(name)
                except Exception:
                    log.warning(f'Failed to cleanup discovery service: {name}')
            raise ex
        return volume_id, False

    def _disconnect_volume(self, volume_id):
        id = format_volume_id(volume_id)
        if id is None:
            raise VolumeException(grpc.StatusCode.INVALID_ARGUMENT,
                                  'Invalid volume ID')
        # Return immediately if the volume is not on our map
        volume = self._volumes.get(id)
        if volume is None:
            return
        self._cleanup_crypto(id)
        # Delete the volume from the map and stop the services it uses
        for name in volume.discovery_services:
            try:
                self._stop_discovery(name)
            except Exception:
                # There's no good way to handle this, so just print an error message and
                # continue
                log.error(f'Failed to stop discovery service: {name}')
        del self._volumes[id]

    @_locked
    def disconnect_volume(self, volume_id):
        """Disconnects a volume connected through discovery service"""
        return self._disconnect_volume(volume_id)

    @_locked
    def set_device(self, volume_id, device_handle):
        """Marks a previously connected volume as being attached to specified device.  This is only
        necessary if the device handle is not known at a time a volume is connected.
        """
        id = format_volume_id(volume_id)
        if id is None:
            raise VolumeException(grpc.StatusCode.INVALID_ARGUMENT,
                                  'Invalid volume ID')
        volume = self._volumes.get(id)
        if volume is None:
            raise VolumeException(grpc.StatusCode.NOT_FOUND,
                                  'Volume could not be found')
        if volume.device_handle is not None and volume.device_handle != device_handle:
            raise VolumeException(grpc.StatusCode.ALREADY_EXISTS,
                                  'Volume is already attached to a different device')
        volume.device_handle = device_handle

    @_locked
    def disconnect_device_volumes(self, device_handle):
        """Disconnects all volumes attached to a specific device"""
        volumes = [i for i, v in self._volumes.items() if v.device_handle == device_handle]
        for volume_id in volumes:
            self._disconnect_volume(volume_id)

    @_locked
    def has_volumes(self, device_handle):
        """Checks whether a given device has volumes attached to it"""
        return next(filter(lambda v: v.device_handle == device_handle,
                           self._volumes.values()), None) is not None
