#!/usr/bin/env python

import os
import re
import sys

comment = re.compile('^\s*#')
assign = re.compile('^\s*([a-zA-Z_]+)\s*(\?)?=\s*([^#]*)')

args = os.environ.copy()
for arg in sys.argv:
    m = assign.match(arg)
    if m:
        var = m.group(1).strip()
        val = m.group(3).strip()
        args[var] = val

# special case for DPDK_DIR, which is short for CONFIG_DPDK_DIR
if 'DPDK_DIR' in args and 'CONFIG_DPDK_DIR' not in args:
    args['CONFIG_DPDK_DIR'] = args['DPDK_DIR']

defs = {}
for config in ('CONFIG', 'CONFIG.local'):
    try:
        with open(config) as f:
            for line in f:
                line = line.strip()
                if not comment.match(line):
                    m = assign.match(line)
                    if m:
                        var = m.group(1).strip()
                        default = m.group(3).strip()
                        val = default
                        if var in args:
                            val = args[var]
                        if default.lower() == 'y' or default.lower() == 'n':
                            if val.lower() == 'y':
                                defs["SPDK_{0}".format(var)] = 1
                            else:
                                defs["SPDK_{0}".format(var)] = 0
                        else:
                            strval = val.replace('"', '\"')
                            defs["SPDK_{0}".format(var)] = strval
    except IOError:
        continue

for key, value in defs.items():
    if value == 0:
        print("#undef {0}".format(key))
    else:
        print("#define {0} {1}".format(key, value))
