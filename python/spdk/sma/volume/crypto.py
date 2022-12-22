#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2022 Intel Corporation.
#  All rights reserved.

import grpc
import logging


log = logging.getLogger(__name__)


class CryptoException(Exception):
    def __init__(self, code, message):
        self.code = code
        self.message = message


class CryptoEngine:
    def __init__(self, name):
        self.name = name

    def init(self, client, params):
        """Initialize crypto engine"""
        self._client = client

    def setup(self, volume_id, key, cipher, key2=None, tweak_mode=None):
        """Set up crypto on a given volume"""
        raise NotImplementedError()

    def cleanup(self, volume_id):
        """
        Disable crypto on a given volume.  If crypto was not configured on that volume, this method
        is a no-op and shouldn't raise any exceptions.
        """
        raise NotImplementedError()

    def verify(self, volume_id, key, cipher, key2=None, tweak_mode=None):
        """
        Verify that specified crypto parameters match those that are currently deployed on a given
        volume.  If key is None, this mehtod ensures that the volume doesn't use crypto.  If
        something is wrong (e.g. keys don't match, different cipher is used, etc.), this method
        raises CryptoException.
        """
        raise NotImplementedError()

    def get_crypto_bdev(self, volume_id):
        """
        Return the name of a crypto bdev on a given volume.  This method might return volume_id if
        crypto engine doesn't create a separate crypto bdev to set up crypto.  If crypto is
        disabled on a given volue, this method returns None.
        """
        raise NotImplementedError()


class CryptoEngineNop(CryptoEngine):
    def __init__(self):
        super().__init__('nop')

    def setup(self, volume_id, key, cipher, key2=None, tweak_mode=None):
        raise CryptoException(grpc.StatusCode.INVALID_ARGUMENT, 'Crypto is disabled')

    def cleanup(self, volume_id):
        pass

    def verify(self, volume_id, key, cipher, key2=None, tweak_mode=None):
        pass

    def get_crypto_bdev(self, volume_id):
        return None


_crypto_engine = None
_crypto_engines = {}


def get_crypto_engine():
    return _crypto_engine


def set_crypto_engine(name):
    global _crypto_engine
    engine = _crypto_engines.get(name)
    if engine is None:
        raise ValueError(f'Unknown crypto engine: {name}')
    log.info(f'Setting crypto engine: {name}')
    _crypto_engine = engine


def register_crypto_engine(engine):
    global _crypto_engines
    _crypto_engines[engine.name] = engine
