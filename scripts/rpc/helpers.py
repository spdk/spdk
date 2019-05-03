deprecated_aliases = {}


def deprecated_alias(old_name):
    def wrap(f):
        def old_f(*args, **kwargs):
            print("{} is deprecated, use {} instead.".format(old_name, f.__name__), file=sys.stderr)
            f(*args, **kwargs)
        deprecated_aliases[old_name] = f.__name__
        return f
    return wrap
