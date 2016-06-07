#!/usr/bin/env python

import re
import sys

comment = re.compile('^\s*#')
assign = re.compile('^\s*([a-zA-Z_]+)\s*(\?)?=\s*([^#]*)')

with open('CONFIG') as f:
    for line in f:
        line = line.strip()
        if not comment.match(line):
            m = assign.match(line)
            if m:
                var = m.group(1).strip()
                default = m.group(3).strip()
                val = default
                for arg in sys.argv:
                    m = assign.match(arg)
                    if m:
                        argvar = m.group(1).strip()
                        argval = m.group(3).strip()
                        if argvar == var:
                            val = argval
                if default.lower() == 'y' or default.lower() == 'n':
                    if val.lower() == 'y':
                        boolval = 1
                    else:
                        boolval = 0
                    print "#define SPDK_{} {}".format(var, boolval)
                else:
                    strval = val.replace('"', '\"')
                    print "#define SPDK_{} \"{}\"".format(var, strval)
