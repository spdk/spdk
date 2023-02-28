#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2022 Intel Corporation.
#  All rights reserved.

import grpc
import logging
import uuid
from spdk.rpc.client import JSONRPCException
from . import crypto
from ..common import format_volume_id
from ..proto import sma_pb2


log = logging.getLogger(__name__)


class CryptoEngineBdev(crypto.CryptoEngine):
    _ciphers = {sma_pb2.VolumeCryptoParameters.AES_CBC: 'AES_CBC',
                sma_pb2.VolumeCryptoParameters.AES_XTS: 'AES_XTS'}

    def __init__(self):
        super().__init__('bdev_crypto')

    def init(self, client, params):
        super().init(client, params)
        # _driver can be None
        self._driver = params.get('driver')

    def setup(self, volume_id, key, cipher, key2=None, tweak_mode=None):
        try:
            with self._client() as client:
                cipher = self._ciphers.get(cipher)
                if cipher is None:
                    raise crypto.CryptoException(grpc.StatusCode.INVALID_ARGUMENT,
                                                 'Invalid volume crypto configuration: bad cipher')
                params = {'base_bdev_name': volume_id,
                          'name': str(uuid.uuid4()),
                          'key': key,
                          'cipher': cipher}
                if self._driver is not None:
                    params['crypto_pmd'] = self._driver
                if key2 is not None:
                    params['key2'] = key2
                if tweak_mode is not None and tweak_mode != sma_pb2.VolumeCryptoParameters.TWEAK_MODE_SIMPLE_LBA:
                    raise crypto.CryptoException(grpc.StatusCode.INVALID_ARGUMENT,
                                                 'Invalid volume crypto configuration: bad tweak_mode')

                log.info('Creating crypto bdev: {} on volume: {}'.format(
                            params['name'], volume_id))
                client.call('bdev_crypto_create', params)
        except JSONRPCException:
            raise crypto.CryptoException(grpc.StatusCode.INTERNAL,
                                         f'Failed to setup crypto for volume: {volume_id}')

    def cleanup(self, volume_id):
        crypto_bdev = self.get_crypto_bdev(volume_id)
        # If there's no crypto bdev set up on top of this volume, we're done
        if crypto_bdev is None:
            return
        try:
            with self._client() as client:
                log.info('Deleting crypto bdev: {} from volume: {}'.format(
                            crypto_bdev, volume_id))
                client.call('bdev_crypto_delete', {'name': crypto_bdev})
        except JSONRPCException:
            raise crypto.CryptoException(grpc.StatusCode.INTERNAL,
                                         'Failed to delete crypto bdev')

    def verify(self, volume_id, key, cipher, key2=None, tweak_mode=None):
        crypto_bdev = self._get_crypto_bdev(volume_id)
        # Key being None/non-None defines whether we expect a bdev_crypto on top of a given volume
        if ((key is None and crypto_bdev is not None) or (key is not None and crypto_bdev is None)):
            raise crypto.CryptoException(grpc.StatusCode.INVALID_ARGUMENT,
                                         'Invalid volume crypto configuration')
        if key is None:
            return
        params = crypto_bdev['driver_specific']['crypto']
        crypto_key = self._get_crypto_key(params['key_name'])
        if crypto_key is None:
            raise crypto.CryptoException(grpc.StatusCode.INVALID_ARGUMENT,
                                         'No key object found')
        cipher = self._ciphers.get(cipher)
        if cipher is None:
            raise crypto.CryptoException(grpc.StatusCode.INVALID_ARGUMENT,
                                         'Invalid volume crypto configuration: bad cipher')
        if crypto_key['cipher'].lower() != cipher.lower():
            raise crypto.CryptoException(grpc.StatusCode.INVALID_ARGUMENT,
                                         'Invalid volume crypto configuration: bad cipher')
        if crypto_key['key'].lower() != key.lower():
            raise crypto.CryptoException(grpc.StatusCode.INVALID_ARGUMENT,
                                         'Invalid volume crypto configuration: bad key')
        if key2 is not None and crypto_key.get('key2', '').lower() != key2.lower():
            raise crypto.CryptoException(grpc.StatusCode.INVALID_ARGUMENT,
                                         'Invalid volume crypto configuration: bad key2')
        if crypto_key['name'].lower() != params['key_name'].lower():
            raise crypto.CryptoException(grpc.StatusCode.INVALID_ARGUMENT,
                                         'Invalid volume crypto configuration: key name does not match')
        if tweak_mode is not None and tweak_mode != sma_pb2.VolumeCryptoParameters.TWEAK_MODE_SIMPLE_LBA:
            raise crypto.CryptoException(grpc.StatusCode.INVALID_ARGUMENT,
                                         'Invalid volume crypto configuration: bad tweak_mode')

    def _get_crypto_bdev(self, volume_id):
        try:
            with self._client() as client:
                bdevs = client.call('bdev_get_bdevs')
                for bdev in [b for b in bdevs if b['product_name'] == 'crypto']:
                    base_name = bdev['driver_specific']['crypto']['base_bdev_name']
                    base_bdev = next(filter(lambda b: b['name'] == base_name, bdevs), None)
                    # Should never really happen, but check it just in case
                    if base_bdev is None:
                        raise crypto.CryptoException(
                                grpc.StatusCode.INTERNAL,
                                'Unexpected crypto configuration: cannot find base bdev')
                    if format_volume_id(base_bdev['uuid']) == volume_id:
                        return bdev
                # There's no crypto bdev set up on top of this volume
                return None
        except JSONRPCException:
            raise crypto.CryptoException(grpc.StatusCode.INTERNAL,
                                         f'Failed to get bdev_crypto for volume: {volume_id}')

    def _get_crypto_key(self, key_name):
        try:
            with self._client() as client:
                _keys = client.call('accel_crypto_keys_get', {'key_name': key_name})
                if _keys is not None:
                    return _keys[0]
                return None
        except JSONRPCException:
            pass

    def get_crypto_bdev(self, volume_id):
        bdev = self._get_crypto_bdev(volume_id)
        if bdev is not None:
            return bdev['name']
        return None
