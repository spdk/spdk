#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

from argparse import ArgumentParser
import dataclasses
import errno
import json
import os
import sys


@dataclasses.dataclass
class PoolConfig:
    small: int = 0
    large: int = 0

    def add(self, other):
        self.small += other.small
        self.large += other.large


class Subsystem:
    _SUBSYSTEMS = {}

    def __init__(self, name, is_target=False):
        self.name = name
        self.is_target = is_target

    def calc(self, config):
        raise NotImplementedError()

    def ask_config(self):
        raise NotImplementedError()

    @staticmethod
    def register(cls):
        subsystem = cls()
        Subsystem._SUBSYSTEMS[subsystem.name] = subsystem

    @staticmethod
    def get(name):
        return Subsystem._SUBSYSTEMS.get(name)

    @staticmethod
    def foreach(cond=lambda _: True):
        for subsystem in Subsystem._SUBSYSTEMS.values():
            if not cond(subsystem):
                continue
            yield subsystem

    @staticmethod
    def get_subsystem_config(config, name):
        for subsystem in config.get('subsystems', []):
            if subsystem['subsystem'] == name:
                return subsystem.get('config', [])

    @staticmethod
    def get_method(config, name):
        return filter(lambda m: m['method'] == name, config)


class AccelSubsystem(Subsystem):
    def __init__(self):
        super().__init__('accel')

    def calc(self, config, mask):
        accel_conf = self.get_subsystem_config(config, 'accel')
        small, large = 128, 16
        opts = next(self.get_method(accel_conf, 'accel_set_options'), {}).get('params')
        if opts is not None:
            small, large = opts['small_cache_size'], opts['large_cache_size']
        cpucnt = mask.bit_count()
        return PoolConfig(small=small * cpucnt, large=large * cpucnt)


class BdevSubsystem(Subsystem):
    def __init__(self):
        super().__init__('bdev')

    def calc(self, config, mask):
        cpucnt = mask.bit_count()
        pool = PoolConfig(small=128 * cpucnt, large=16 * cpucnt)
        pool.add(self.get('accel').calc(config, mask))
        return pool


class NvmfSubsystem(Subsystem):
    def __init__(self):
        super().__init__('nvmf', True)

    def calc(self, config, mask):
        transports = [*self.get_method(self.get_subsystem_config(config, 'nvmf'),
                                       'nvmf_create_transport')]
        small_bufsize = next(self.get_method(self.get_subsystem_config(config, 'iobuf'),
                                             'iobuf_set_options'),
                             {'params': {'small_bufsize': 8192}})['params']['small_bufsize']
        cpucnt = mask.bit_count()
        max_u32 = (1 << 32) - 1

        pool = PoolConfig()
        if len(transports) == 0:
            return pool

        # Add bdev layer's pools acquired on the nvmf threads
        pool.add(self.get('bdev').calc(config, mask))
        for transport in transports:
            params = transport['params']
            buf_cache_size = params['buf_cache_size']
            io_unit_size = params['io_unit_size']
            num_shared_buffers = params['num_shared_buffers']
            if buf_cache_size == 0:
                continue

            if buf_cache_size == max_u32:
                buf_cache_size = (num_shared_buffers * 3 // 4) // cpucnt
            if io_unit_size <= small_bufsize:
                large = 0
            else:
                large = buf_cache_size
            pool.add(PoolConfig(small=buf_cache_size * cpucnt, large=large * cpucnt))
        return pool


class UblkSubsystem(Subsystem):
    def __init__(self):
        super().__init__('ublk', True)

    def calc(self, config, mask):
        ublk_conf = self.get_subsystem_config(config, 'ublk')
        target = next(iter([m for m in ublk_conf if m['method'] == 'ublk_create_target']),
                      {}).get('params')
        pool = PoolConfig()
        if target is None:
            return pool
        # Add bdev layer's pools acquired on the ublk threads
        pool.add(self.get('bdev').calc(config, mask))
        cpucnt = int(target['cpumask'], 0).bit_count()
        return PoolConfig(small=128 * cpucnt, large=32 * cpucnt)


def parse_config(config, mask):
    pool = PoolConfig()
    for subsystem in Subsystem.foreach(lambda s: s.is_target):
        subcfg = Subsystem.get_subsystem_config(config, subsystem.name)
        if subcfg is None:
            continue
        pool.add(subsystem.calc(config, mask))
    return pool


def main():
    Subsystem.register(AccelSubsystem)
    Subsystem.register(BdevSubsystem)
    Subsystem.register(NvmfSubsystem)
    Subsystem.register(UblkSubsystem)

    appname = sys.argv[0]
    parser = ArgumentParser(description='Utility to help calculate minimum iobuf pool size based '
                            'on app\'s config. '
                            'This script will only calculate the minimum values required to '
                            'populate the per-thread caches.  Most users will usually want to use '
                            'larger values to leave some buffers in the global pool.')
    parser.add_argument('--core-mask', '-m', help='Core mask', type=lambda v: int(v, 0))
    parser.add_argument('--config', '-c', help='Config file', required=True)

    args = parser.parse_args()
    try:
        with open(args.config, 'r') as f:
            pool = parse_config(json.load(f), args.core_mask)
            print('This script will only calculate the minimum values required to populate the '
                  'per-thread caches.\nMost users will usually want to use larger values to leave '
                  'some buffers in the global pool.\n')
            print(f'Minimum small pool size: {pool.small}')
            print(f'Minimum large pool size: {pool.large}')
    except FileNotFoundError:
        print(f'{appname}: {args.config}: {os.strerror(errno.ENOENT)}')
        sys.exit(1)
    except json.decoder.JSONDecodeError:
        print(f'{appname}: {args.config}: {os.strerror(errno.EINVAL)}')
        sys.exit(1)
    except KeyboardInterrupt:
        sys.exit(1)


main()
