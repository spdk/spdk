#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2019 Intel Corporation.
#  All rights reserved.

import functools
import sys
from typing import Dict

deprecated_aliases: Dict[str, str] = {}


def check_called_name(name):
    if name in deprecated_aliases:
        print("{} is deprecated, use {} instead.".format(name, deprecated_aliases[name]), file=sys.stderr)


def deprecated_alias(old_name):
    def wrap(f):
        def old_f(*args, **kwargs):
            ret = f(*args, **kwargs)
            print("{} is deprecated, use {} instead.".format(old_name, f.__name__), file=sys.stderr)
            return ret
        old_f.__name__ = old_name
        deprecated_aliases[old_name] = f.__name__
        setattr(sys.modules[f.__module__], old_name, old_f)
        return f
    return wrap


def deprecated_method(method):
    method.deprecated_warning = False

    @functools.wraps(method)
    def wrap(*args, **kwargs):
        if not method.deprecated_warning:
            print(f'{method.__name__} is deprecated, use JSONRPCClient directly', file=sys.stderr)
            method.deprecated_warning = True
        return method(*args, **kwargs)
    return wrap


def hint_rpc_name(parser):
    try:
        from CommandNotFound.CommandNotFound import similar_words as similar_rpcs  # type: ignore[import]
    except (ImportError, ModuleNotFoundError):
        return parser

    def error(msg):
        srpcs = set()
        e = msg

        if "choose from " in msg:
            fmsg = msg.split("choose from ")

            bad_arg = fmsg[0].split("'")[1]
            rpcs = fmsg[1].strip().strip(")").replace("'", "").split(", ")

            for similar_rpc in similar_rpcs(bad_arg):
                if similar_rpc in rpcs:
                    srpcs.add(similar_rpc)

            if srpcs:
                e = f"'{bad_arg}' not recognized, did you mean: {', '.join(srpcs)}?"

        print(e, file=sys.stderr)
        sys.exit(2)

    parser.error = error
    return parser
