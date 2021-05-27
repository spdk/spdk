#!/usr/bin/env python3

from argparse import ArgumentParser
from dataclasses import dataclass
from typing import Dict, List, TypeVar
import json
import os
import re
import subprocess
import sys
import tempfile


@dataclass
class DTraceArgument:
    """Describes a DTrace probe (usdt) argument"""
    name: str
    pos: int
    type: type


@dataclass
class DTraceProbe:
    """Describes a DTrace probe (usdt) point"""
    name: str
    args: Dict[str, DTraceArgument]

    def __init__(self, name, args):
        self.name = name
        self.args = {a.name: a for a in args}


@dataclass
class DTraceEntry:
    """Describes a single DTrace probe invocation"""
    name: str
    args: Dict[str, TypeVar('ArgumentType', str, int)]

    def __init__(self, probe, args):
        valmap = {int: lambda x: int(x, 16),
                  str: lambda x: x.strip().strip("'")}
        self.name = probe.name
        self.args = {}
        for name, value in args.items():
            arg = probe.args.get(name)
            if arg is None:
                raise ValueError(f'Unexpected argument: {name}')
            self.args[name] = valmap[arg.type](value)


class DTrace:
    """Generates bpftrace script based on the supplied probe points, parses its
    output and stores is as a list of DTraceEntry sorted by their tsc.
    """
    def __init__(self, probes, file=None):
        self._avail_probes = self._list_probes()
        self._probes = {p.name: p for p in probes}
        self.entries = self._parse(file) if file is not None else []
        # Sanitize the probe definitions
        for probe in probes:
            if probe.name not in self._avail_probes:
                raise ValueError(f'Couldn\'t find probe: "{probe.name}"')
            for arg in probe.args.values():
                if arg.pos >= self._avail_probes[probe.name]:
                    raise ValueError('Invalid probe argument position')
                if arg.type not in (int, str):
                    raise ValueError('Invalid argument type')

    def _parse(self, file):
        regex = re.compile(r'(\w+): (.*)')
        entries = []

        for line in file.readlines():
            match = regex.match(line)
            if match is None:
                continue
            name, args = match.groups()
            probe = self._probes.get(name)
            # Skip the line if we don't recognize the probe name
            if probe is None:
                continue
            entries.append(DTraceEntry(probe, args=dict(a.strip().split('=')
                                                        for a in args.split(','))))
        entries.sort(key=lambda e: e.args['tsc'])
        return entries

    def _list_probes(self):
        files = subprocess.check_output(['git', 'ls-files', '*.[ch]',
                                        ':!:include/spdk_internal/usdt.h'])
        files = filter(lambda f: len(f) > 0, str(files, 'ascii').split('\n'))
        regex = re.compile(r'SPDK_DTRACE_PROBE([0-9]*)\((\w+)')
        probes = {}

        for fname in files:
            with open(fname, 'r') as file:
                for match in regex.finditer(file.read()):
                    nargs, name = match.group(1), match.group(2)
                    nargs = int(nargs) if len(nargs) > 0 else 0
                    # Add one to accommodate for the tsc being the first arg
                    probes[name] = nargs + 1
        return probes

    def _gen_usdt(self, probe):
        usdt = (f'usdt:__EXE__:{probe.name} {{' +
                f'printf("{probe.name}: ')
        args = probe.args
        if len(args) > 0:
            argtype = {int: '0x%lx', str: '\'%s\''}
            argcast = {int: lambda x: x, str: lambda x: f'str({x})'}
            argstr = [f'{a.name}={argtype[a.type]}' for a in args.values()]
            argval = [f'{argcast[a.type](f"arg{a.pos}")}' for a in args.values()]
            usdt += ', '.join(argstr) + '\\n", ' + ', '.join(argval)
        else:
            usdt += '\\n"'
        usdt += ');}'
        return usdt

    def generate(self):
        return '\n'.join([self._gen_usdt(p) for p in self._probes.values()])

    def record(self, pid):
        with tempfile.NamedTemporaryFile(mode='w+') as script:
            script.write(self.generate())
            script.flush()
            try:
                subprocess.run([f'{os.path.dirname(__file__)}/../bpftrace.sh',
                                f'{pid}', f'{script.name}'])
            except KeyboardInterrupt:
                pass


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


def build_dtrace():
    return DTrace([
        DTraceProbe(
            name='nvmf_poll_group_add_qpair',
            args=[DTraceArgument(name='tsc', pos=0, type=int),
                  DTraceArgument(name='qpair', pos=1, type=int),
                  DTraceArgument(name='thread', pos=2, type=int)]),
        DTraceProbe(
            name='nvmf_poll_group_remove_qpair',
            args=[DTraceArgument(name='tsc', pos=0, type=int),
                  DTraceArgument(name='qpair', pos=1, type=int),
                  DTraceArgument(name='thread', pos=2, type=int)]),
        DTraceProbe(
            name='nvmf_ctrlr_add_qpair',
            args=[DTraceArgument(name='tsc', pos=0, type=int),
                  DTraceArgument(name='qpair', pos=1, type=int),
                  DTraceArgument(name='qid', pos=2, type=int),
                  DTraceArgument(name='subnqn', pos=3, type=str),
                  DTraceArgument(name='hostnqn', pos=4, type=str)])])


def main(argv):
    parser = ArgumentParser(description='SPDK trace annotation script')
    parser.add_argument('-i', '--input',
                        help='JSON-formatted trace file produced by spdk_trace app')
    parser.add_argument('-g', '--generate', help='Generate bpftrace script', action='store_true')
    parser.add_argument('-r', '--record', help='Record BPF traces on PID', metavar='PID', type=int)
    args = parser.parse_args(argv)

    if args.generate:
        print(build_dtrace().generate())
    elif args.record:
        build_dtrace().record(args.record)
    else:
        file = open(args.input, 'r') if args.input is not None else sys.stdin
        Trace(file).print()


if __name__ == '__main__':
    main(sys.argv[1:])
