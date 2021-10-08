#!/usr/bin/env python3

from argparse import ArgumentParser
import os
import re
import subprocess
import sys


class TraceProcess:
    def __init__(self, pid):
        self._path = os.readlink(f'/proc/{pid}/exe')
        self._pid = pid
        self._probes = self._init_probes()

    def _init_probes(self):
        lines = subprocess.check_output(['bpftrace', '-l', '-p', str(self._pid)], text=True)
        probes = {}
        for line in lines.split('\n'):
            parts = line.split(':')
            if len(parts) < 3:
                continue
            ptype, path, function = parts[0], parts[1], parts[-1]
            probes[(ptype, function)] = path
        return probes

    def fixup(self, script):
        pregs = [re.compile(r'({}):__EXE__:(\w+)'.format(ptype)) for ptype in ['usdt', 'uprobe']]
        with open(script, 'r') as file:
            lines = file.readlines()
        result = ''
        for line in lines:
            for regex in pregs:
                match = regex.match(line)
                if match is not None:
                    ptype, function = match.groups()
                    path = self._probes.get((ptype, function), self._path)
                    line = line.replace('__EXE__', path)
                    break
            result += line.replace('__EXE__', self._path).replace('__PID__', str(self._pid))
        return result


if __name__ == '__main__':
    parser = ArgumentParser(description='bpftrace script generator replacing special ' +
                            'variables in the scripts with appropriate values')
    parser.add_argument('-p', '--pid', type=int, required=True, help='PID of a traced process')
    parser.add_argument('scripts', metavar='SCRIPTS', type=str, nargs='+',
                        help='bpftrace scripts to process')
    args = parser.parse_args(sys.argv[1:])
    proc = TraceProcess(args.pid)
    for script in args.scripts:
        print(proc.fixup(script))
