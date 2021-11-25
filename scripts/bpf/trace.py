#!/usr/bin/env python3

from argparse import ArgumentParser
from dataclasses import dataclass, field
from itertools import islice
from typing import Dict, List, TypeVar
import ctypes as ct
import ijson
import magic
import os
import re
import subprocess
import sys
import tempfile

TSC_MAX = (1 << 64) - 1
UCHAR_MAX = (1 << 8) - 1
TRACE_MAX_LCORE = 128
TRACE_MAX_GROUP_ID = 16
TRACE_MAX_TPOINT_ID = TRACE_MAX_GROUP_ID * 64
TRACE_MAX_ARGS_COUNT = 5
TRACE_MAX_RELATIONS = 16
TRACE_INVALID_OBJECT = (1 << 64) - 1
OBJECT_NONE = 0
OWNER_NONE = 0


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
    object_type: int
    owner_type: int
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
    related: str


class TraceProvider:
    """Defines interface for objects providing traces and tracepoint definitions"""

    def tpoints(self):
        """Returns tracepoint definitions as a dict of (tracepoint_name, tracepoint)"""
        raise NotImplementedError()

    def entries(self):
        """Generator returning subsequent trace entries"""
        raise NotImplementedError()

    def tsc_rate(self):
        """Returns the TSC rate that was in place when traces were collected"""
        raise NotImplementedError()


class JsonProvider(TraceProvider):
    """Trace provider based on JSON-formatted output produced by spdk_trace app"""
    def __init__(self, file):
        self._parser = ijson.parse(file)
        self._tpoints = {}
        self._parse_defs()

    def _parse_tpoints(self, tpoints):
        for tpoint in tpoints:
            tpoint_id = tpoint['id']
            self._tpoints[tpoint_id] = Tracepoint(
                name=tpoint['name'], id=tpoint_id,
                new_object=tpoint['new_object'], object_type=OBJECT_NONE,
                owner_type=OWNER_NONE,
                args=[TracepointArgument(name=a['name'],
                                         argtype=a['type'])
                      for a in tpoint.get('args', [])])

    def _parse_defs(self):
        builder = None
        for prefix, event, value in self._parser:
            # If we reach entries array, there are no more tracepoint definitions
            if prefix == 'entries':
                break
            elif prefix == 'tsc_rate':
                self._tsc_rate = value
                continue

            if (prefix, event) == ('tpoints', 'start_array'):
                builder = ijson.ObjectBuilder()
            if builder is not None:
                builder.event(event, value)
            if (prefix, event) == ('tpoints', 'end_array'):
                self._parse_tpoints(builder.value)
                builder = None

    def _parse_entry(self, entry):
        tpoint = self._tpoints[entry['tpoint']]
        obj = entry.get('object', {})
        return TraceEntry(tpoint=tpoint, lcore=entry['lcore'], tsc=entry['tsc'],
                          size=entry.get('size'), object_id=obj.get('id'),
                          object_ptr=obj.get('value'), related=entry.get('related'),
                          time=obj.get('time'), poller=entry.get('poller'),
                          args={n.name: v for n, v in zip(tpoint.args, entry.get('args', []))})

    def tsc_rate(self):
        return self._tsc_rate

    def tpoints(self):
        return self._tpoints

    def entries(self):
        builder = None
        for prefix, event, value in self._parser:
            if (prefix, event) == ('entries.item', 'start_map'):
                builder = ijson.ObjectBuilder()
            if builder is not None:
                builder.event(event, value)
            if (prefix, event) == ('entries.item', 'end_map'):
                yield self._parse_entry(builder.value)
                builder = None


class CParserOpts(ct.Structure):
    _fields_ = [('filename', ct.c_char_p),
                ('mode', ct.c_int),
                ('lcore', ct.c_uint16)]


class CTraceOwner(ct.Structure):
    _fields_ = [('type', ct.c_uint8),
                ('id_prefix', ct.c_char)]


class CTraceObject(ct.Structure):
    _fields_ = [('type', ct.c_uint8),
                ('id_prefix', ct.c_char)]


class CTpointArgument(ct.Structure):
    _fields_ = [('name', ct.c_char * 14),
                ('type', ct.c_uint8),
                ('size', ct.c_uint8)]


class CTpointRelatedObject(ct.Structure):
    _fields_ = [('object_type', ct.c_uint8),
                ('arg_index', ct.c_uint8)]


class CTracepoint(ct.Structure):
    _fields_ = [('name', ct.c_char * 24),
                ('tpoint_id', ct.c_uint16),
                ('owner_type', ct.c_uint8),
                ('object_type', ct.c_uint8),
                ('new_object', ct.c_uint8),
                ('num_args', ct.c_uint8),
                ('args', CTpointArgument * TRACE_MAX_ARGS_COUNT),
                ('related_objects', CTpointRelatedObject * TRACE_MAX_RELATIONS)]


class CTraceFlags(ct.Structure):
    _fields_ = [('tsc_rate', ct.c_uint64),
                ('tpoint_mask', ct.c_uint64 * TRACE_MAX_GROUP_ID),
                ('owner', CTraceOwner * (UCHAR_MAX + 1)),
                ('object', CTraceObject * (UCHAR_MAX + 1)),
                ('tpoint', CTracepoint * TRACE_MAX_TPOINT_ID)]


class CTraceEntry(ct.Structure):
    _fields_ = [('tsc', ct.c_uint64),
                ('tpoint_id', ct.c_uint16),
                ('poller_id', ct.c_uint16),
                ('size', ct.c_uint32),
                ('object_id', ct.c_uint64)]


class CTraceParserArgument(ct.Union):
    _fields_ = [('integer', ct.c_uint64),
                ('pointer', ct.c_void_p),
                ('string', ct.c_char * (UCHAR_MAX + 1))]


class CTraceParserEntry(ct.Structure):
    _fields_ = [('entry', ct.POINTER(CTraceEntry)),
                ('object_index', ct.c_uint64),
                ('object_start', ct.c_uint64),
                ('lcore', ct.c_uint16),
                ('related_index', ct.c_uint64),
                ('related_type', ct.c_uint8),
                ('args', CTraceParserArgument * TRACE_MAX_ARGS_COUNT)]


class NativeProvider(TraceProvider):
    """Trace provider based on SPDK's trace library"""
    def __init__(self, file):
        self._setup_binding(file.name)
        self._parse_defs()

    def __del__(self):
        if hasattr(self, '_parser'):
            self._lib.spdk_trace_parser_cleanup(self._parser)

    def _setup_binding(self, filename):
        self._lib = ct.CDLL('build/lib/libspdk_trace_parser.so')
        self._lib.spdk_trace_parser_init.restype = ct.c_void_p
        self._lib.spdk_trace_parser_init.errcheck = lambda r, *_: ct.c_void_p(r)
        self._lib.spdk_trace_parser_get_flags.restype = ct.POINTER(CTraceFlags)
        opts = CParserOpts(filename=bytes(filename, 'ascii'), mode=0,
                           lcore=TRACE_MAX_LCORE)
        self._parser = self._lib.spdk_trace_parser_init(ct.byref(opts))
        if not self._parser:
            raise ValueError('Failed to construct SPDK trace parser')

    def _parse_tpoints(self, tpoints):
        self._tpoints = {}
        for tpoint in tpoints:
            if len(tpoint.name) == 0:
                continue
            self._tpoints[tpoint.tpoint_id] = Tracepoint(
                name=str(tpoint.name, 'ascii'), object_type=tpoint.object_type,
                owner_type=tpoint.owner_type, id=tpoint.tpoint_id,
                new_object=bool(tpoint.new_object),
                args=[TracepointArgument(name=str(a.name, 'ascii'), argtype=a.type)
                      for a in tpoint.args[:tpoint.num_args]])

    def _parse_defs(self):
        flags = self._lib.spdk_trace_parser_get_flags(self._parser)
        self._tsc_rate = flags.contents.tsc_rate
        self._parse_tpoints(flags.contents.tpoint)

        def conv_objs(arr):
            return {int(o.type): str(o.id_prefix, 'ascii') for o in arr if o.id_prefix != b'\x00'}
        self._owners = conv_objs(flags.contents.owner)
        self._objects = conv_objs(flags.contents.object)

    def tsc_rate(self):
        return self._tsc_rate

    def tpoints(self):
        return self._tpoints

    def entries(self):
        pe = CTraceParserEntry()
        argconv = {TracepointArgument.TYPE_INT: lambda a: a.integer,
                   TracepointArgument.TYPE_PTR: lambda a: int(a.pointer or 0),
                   TracepointArgument.TYPE_STR: lambda a: str(a.string, 'ascii')}

        while self._lib.spdk_trace_parser_next_entry(self._parser, ct.byref(pe)):
            entry = pe.entry.contents
            lcore = pe.lcore
            tpoint = self._tpoints[entry.tpoint_id]
            args = {a.name: argconv[a.argtype](pe.args[i]) for i, a in enumerate(tpoint.args)}

            if tpoint.object_type != OBJECT_NONE:
                if pe.object_index != TRACE_INVALID_OBJECT:
                    object_id = '{}{}'.format(self._objects[tpoint.object_type], pe.object_index)
                    ts = entry.tsc - pe.object_start
                else:
                    object_id, ts = 'n/a', None
            elif entry.object_id != 0:
                object_id, ts = '{:x}'.format(entry.object_id), None
            else:
                object_id, ts = None, None

            if tpoint.owner_type != OWNER_NONE:
                poller_id = '{}{:02}'.format(self._owners[tpoint.owner_type], entry.poller_id)
            else:
                poller_id = None

            if pe.related_type != OBJECT_NONE:
                related = '{}{}'.format(self._objects[pe.related_type], pe.related_index)
            else:
                related = None

            yield TraceEntry(tpoint=tpoint, lcore=lcore, tsc=entry.tsc,
                             size=entry.size, object_id=object_id,
                             object_ptr=entry.object_id, poller=poller_id, time=ts,
                             args=args, related=related)


class Trace:
    """Stores, parses, and prints out SPDK traces"""
    def __init__(self, file):
        if file == sys.stdin or magic.from_file(file.name, mime=True) == 'application/json':
            self._provider = JsonProvider(file)
        else:
            self._provider = NativeProvider(file)
        self._objects = []
        self._argfmt = {TracepointArgument.TYPE_PTR: lambda a: f'0x{a:x}'}
        self.tpoints = self._provider.tpoints()

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
            return ((tsc - off) * 10 ** 6) / self._provider.tsc_rate()

        offset = None
        for e in self._provider.entries():
            offset = e.tsc if offset is None else offset
            timestamp = get_us(e.tsc, offset)
            diff = get_us(e.time, 0) if e.time is not None else None
            args = ', '.join(self._format_args(e))
            related = ' (' + e.related + ')' if e.related is not None else ''

            print(('{:3} {:16.3f} {:3} {:24} {:12}'.format(
                e.lcore, timestamp, e.poller if e.poller is not None else '',
                e.tpoint.name, f'size: {e.size}' if e.size else '') +
                (f'id: {e.object_id + related:12} ' if e.object_id is not None else '') +
                (f'time: {diff:<8.3f} ' if diff is not None else '') +
                args).rstrip())


class SPDKObject:
    """Describes a specific type of an SPDK objects (e.g. qpair, thread, etc.)"""
    @dataclass
    class Lifetime:
        """Describes a lifetime and properties of a particular SPDK object."""
        begin: int
        end: int
        ptr: int
        properties: dict = field(default_factory=dict)

    def __init__(self, trace: Trace, tpoints: List[str]):
        self.tpoints = {}
        for name in tpoints:
            tpoint = next((t for t in trace.tpoints.values() if t.name == name), None)
            if tpoint is None:
                # Some tpoints might be undefined if configured without specific subsystems
                continue
            self.tpoints[tpoint.id] = tpoint

    def _annotate(self, entry: TraceEntry):
        """Abstract annotation method to be implemented by subclasses."""
        raise NotImplementedError()

    def annotate(self, entry: TraceEntry):
        """Annotates a tpoint entry and returns a dict indexed by argname with values representing
        various object properties.  For instance, {"qpair": {"qid": 1, "subnqn": "nqn"}} could be
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
            'RDMA_REQ_COMPLETED',
            'TCP_REQ_NEW',
            'TCP_REQ_NEED_BUFFER',
            'TCP_REQ_TX_H_TO_C',
            'TCP_REQ_RDY_TO_EXECUTE',
            'TCP_REQ_EXECUTING',
            'TCP_REQ_EXECUTED',
            'TCP_REQ_RDY_TO_COMPLETE',
            'TCP_REQ_TRANSFER_C2H',
            'TCP_REQ_COMPLETED',
            'TCP_WRITE_START',
            'TCP_WRITE_DONE',
            'TCP_READ_DONE',
            'TCP_REQ_AWAIT_R2T_ACK'])
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
                        help='Trace file to annotate (either JSON generated by spdk_trace or ' +
                             'raw binary produced by the SPDK application itself)')
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
    # In order for the changes to LD_LIBRARY_PATH to be visible to the loader,
    # they need to be applied before starting a process, so we need to
    # re-execute the script after updating it.
    if os.environ.get('SPDK_BPF_TRACE_PY') is None:
        rootdir = f'{os.path.dirname(__file__)}/../..'
        os.environ['LD_LIBRARY_PATH'] = ':'.join([os.environ.get('LD_LIBRARY_PATH', ''),
                                                  f'{rootdir}/build/lib'])
        os.environ['SPDK_BPF_TRACE_PY'] = '1'
        os.execv(sys.argv[0], sys.argv)
    else:
        try:
            main(sys.argv[1:])
        except (KeyboardInterrupt, BrokenPipeError):
            pass
