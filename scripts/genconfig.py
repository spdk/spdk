#!/usr/bin/env python3

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

defs = {}
try:
    with open("mk/config.mk") as f:
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
    print("mk/config.mk not found")

for key, value in sorted(defs.items()):
    if value == 0:
        print("#undef {0}".format(key))
    else:
        print("#define {0} {1}".format(key, value))
