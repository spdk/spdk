def deprecated_alias(old_name):
    def wrap(f):
        def old_f(*args, **kwargs):
            print("{} is deprecated, use {} instead.".format(old_name, f.__name__), sys.stderr)
            f(*args, **kwargs)
        globals()[old_name] = old_f
        return f
    return wrap
