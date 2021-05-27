#!/usr/bin/env python3

from argparse import ArgumentParser
from dataclasses import dataclass, field
from itertools import islice
from typing import Dict, List, TypeVar
import json
import os
import re
import subprocess
import sys
import tempfile

TSC_MAX = (1 << 64) - 1


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
        self._objects = []
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

    def _annotate_args(self, entry):
        annotations = {}
        for obj in self._objects:
            current = obj.annotate(entry)
            if current is None:
                continue
            annotations.update(current)
        return annotations

    def _format_args(self, entry):
        annotations = self._annotate_args(entry)
        args = []
        for arg, (name, value) in zip(entry.tpoint.args, entry.args.items()):
            annot = annotations.get(name)
            if annot is not None:
                args.append('{}({})'.format(name, ', '.join(f'{n}={v}' for n, v in annot.items())))
            else:
                args.append('{}: {}'.format(name, self._argfmt.get(arg.argtype,
                                                                   lambda a: a)(value)))
        return args

    def register_object(self, obj):
        self._objects.append(obj)

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


class SPDKObject:
    """Describes a specific type of an SPDK objects (e.g. qpair, thread, etc.)"""
    @dataclass
    class Lifetime:
        """Describes a lifetime and properites of a particular SPDK object."""
        begin: int
        end: int
        ptr: int
        properties: dict = field(default_factory=dict)

    def __init__(self, trace: Trace, tpoints: List[str]):
        self.tpoints = {}
        for name in tpoints:
            tpoint = next((t for t in trace.tpoints.values() if t.name == name), None)
            if tpoint is None:
                # Some tpoints might be undefined if configured without specific subystems
                continue
            self.tpoints[tpoint.id] = tpoint

    def _annotate(self, entry: TraceEntry):
        """Abstract annotation method to be implemented by subclasses."""
        raise NotImplementedError()

    def annotate(self, entry: TraceEntry):
        """Annotates a tpoint entry and returns a dict indexed by argname with values representing
        various object properites.  For instance, {"qpair": {"qid": 1, "subnqn": "nqn"}} could be
        returned to annotate an argument called "qpair" with two items: "qid" and "subnqn".
        """
        if entry.tpoint.id not in self.tpoints:
            return None
        return self._annotate(entry)


class QPair(SPDKObject):
    def __init__(self, trace: Trace, dtrace: DTrace):
        super().__init__(trace, tpoints=[
            'RDMA_REQ_NEW',
            'RDMA_REQ_NEED_BUFFER',
            'RDMA_REQ_TX_PENDING_C2H',
            'RDMA_REQ_TX_PENDING_H2C',
            'RDMA_REQ_TX_H2C',
            'RDMA_REQ_RDY_TO_EXECUTE',
            'RDMA_REQ_EXECUTING',
            'RDMA_REQ_EXECUTED',
            'RDMA_REQ_RDY_TO_COMPL',
            'RDMA_REQ_COMPLETING_C2H',
            'RDMA_REQ_COMPLETING',
            'RDMA_REQ_COMPLETED'])
        self._objects = []
        self._find_objects(dtrace.entries)

    def _find_objects(self, dprobes):
        def probe_match(probe, other):
            return probe.args['qpair'] == other.args['qpair']

        for i, dprobe in enumerate(dprobes):
            if dprobe.name != 'nvmf_poll_group_add_qpair':
                continue
            # We've found a new qpair, now find the probe indicating its destruction
            last_idx, last = next((((i + j + 1), d) for j, d in enumerate(islice(dprobes, i, None))
                                   if d.name == 'nvmf_poll_group_remove_qpair' and
                                   probe_match(d, dprobe)), (None, None))
            obj = SPDKObject.Lifetime(begin=dprobe.args['tsc'],
                                      end=last.args['tsc'] if last is not None else TSC_MAX,
                                      ptr=dprobe.args['qpair'],
                                      properties={'ptr': hex(dprobe.args['qpair']),
                                                  'thread': dprobe.args['thread']})
            for other in filter(lambda p: probe_match(p, dprobe), dprobes[i:last_idx]):
                if other.name == 'nvmf_ctrlr_add_qpair':
                    for prop in ['qid', 'subnqn', 'hostnqn']:
                        obj.properties[prop] = other.args[prop]
            self._objects.append(obj)

    def _annotate(self, entry):
        qpair = entry.args.get('qpair')
        if qpair is None:
            return None
        for obj in self._objects:
            if obj.ptr == qpair and obj.begin <= entry.tsc <= obj.end:
                return {'qpair': obj.properties}
        return None


def build_dtrace(file=None):
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
                  DTraceArgument(name='hostnqn', pos=4, type=str)])], file)


def print_trace(trace_file, dtrace_file):
    dtrace = build_dtrace(dtrace_file)
    trace = Trace(trace_file)
    trace.register_object(QPair(trace, dtrace))
    trace.print()


def main(argv):
    parser = ArgumentParser(description='SPDK trace annotation script')
    parser.add_argument('-i', '--input',
                        help='JSON-formatted trace file produced by spdk_trace app')
    parser.add_argument('-g', '--generate', help='Generate bpftrace script', action='store_true')
    parser.add_argument('-r', '--record', help='Record BPF traces on PID', metavar='PID', type=int)
    parser.add_argument('-b', '--bpftrace', help='BPF trace script to use for annotations')
    args = parser.parse_args(argv)

    if args.generate:
        print(build_dtrace().generate())
    elif args.record:
        build_dtrace().record(args.record)
    else:
        print_trace(open(args.input, 'r') if args.input is not None else sys.stdin,
                    open(args.bpftrace) if args.bpftrace is not None else None)


if __name__ == '__main__':
    main(sys.argv[1:])
