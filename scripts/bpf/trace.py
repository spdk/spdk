#!/usr/bin/env python3

from argparse import ArgumentParser
from dataclasses import dataclass
from typing import Dict, List, TypeVar
import json
import sys


@dataclass
class TracepointArgument:
    """Describes an SPDK tracepoint argument"""
    TYPE_INT = 0
    TYPE_PTR = 1
    TYPE_STR = 2
    name: str
    argtype: int


@dataclass
class Tracepoint:
    """Describes an SPDK tracepoint, equivalent to struct spdk_trace_tpoint"""
    name: str
    id: int
    new_object: bool
    args: List[TracepointArgument]


@dataclass
class TraceEntry:
    """Describes an SPDK tracepoint entry, equivalent to struct spdk_trace_entry"""
    lcore: int
    tpoint: Tracepoint
    tsc: int
    poller: str
    size: int
    object_id: str
    object_ptr: int
    time: int
    args: Dict[str, TypeVar('ArgumentType', str, int)]


class Trace:
    """Stores, parses, and prints out SPDK traces"""
    def __init__(self, file):
        self._json = json.load(file)
        self._argfmt = {TracepointArgument.TYPE_PTR: lambda a: f'0x{a:x}'}
        self.tpoints = {t.id: t for t in self._parse_tpoints()}
        self.tsc_rate = self._json['tsc_rate']

    def _parse_tpoints(self):
        for tpoint in self._json.get('tpoints', []):
            yield Tracepoint(
                name=tpoint['name'], id=tpoint['id'],
                new_object=tpoint['new_object'],
                args=[TracepointArgument(name=a['name'],
                                         argtype=a['type'])
                      for a in tpoint.get('args', [])])

    def _parse_entry(self, entry):
        tpoint = self.tpoints[entry['tpoint']]
        obj = entry.get('object', {})
        return TraceEntry(tpoint=tpoint, lcore=entry['lcore'], tsc=entry['tsc'],
                          size=entry.get('size'), object_id=obj.get('id'),
                          object_ptr=obj.get('value'), time=obj.get('time'),
                          poller=entry.get('poller'),
                          args={n.name: v for n, v in zip(tpoint.args, entry.get('args', []))})

    def _entries(self):
        for entry in self._json.get('entries', []):
            yield self._parse_entry(entry)

    def _format_args(self, entry):
        args = []
        for arg, (name, value) in zip(entry.tpoint.args, entry.args.items()):
            args.append('{}: {}'.format(name, self._argfmt.get(arg.argtype,
                                                               lambda a: a)(value)))
        return args

    def print(self):
        def get_us(tsc, off):
            return ((tsc - off) * 10 ** 6) / self.tsc_rate

        offset = None
        for e in self._entries():
            offset = e.tsc if offset is None else offset
            timestamp = get_us(e.tsc, offset)
            diff = get_us(e.time, 0) if e.time is not None else None
            args = ', '.join(self._format_args(e))
            fields = [
                f'{e.lcore:3}',
                f'{timestamp:16.3f}',
                f'{e.poller:3}' if e.poller is not None else ' ' * 3,
                f'{e.tpoint.name:24}',
                f'size: {e.size:6}' if e.size is not None else ' ' * (len('size: ') + 6),
                f'id: {e.object_id:8}' if e.object_id is not None else None,
                f'time: {diff:<8.3f}' if diff is not None else None,
                args
            ]

            print(' '.join([*filter(lambda f: f is not None, fields)]).rstrip())


def main(argv):
    parser = ArgumentParser(description='SPDK trace annotation script')
    parser.add_argument('-i', '--input',
                        help='JSON-formatted trace file produced by spdk_trace app')
    args = parser.parse_args(argv)

    file = open(args.input, 'r') if args.input is not None else sys.stdin
    Trace(file).print()


if __name__ == '__main__':
    main(sys.argv[1:])
