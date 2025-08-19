#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2019 Intel Corporation.
#  All rights reserved.

import sys

deprecated_aliases = {}


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

    def wrap(*args, **kwargs):
        if not method.deprecated_warning:
            print(f'{method.__name__} is deprecated, use JSONRPCClient directly', file=sys.stderr)
            method.deprecated_warning = True
        return method(*args, **kwargs)
    return wrap
