args_global = ['server_addr', 'port', 'timeout', 'verbose', 'dry_run', 'conn_retries',
               'is_server', 'rpc_plugin', 'called_rpc_name', 'func', 'client']


def strip_globals(kwargs):
    for arg in args_global:
        kwargs.pop(arg, None)


def remove_null(kwargs):
    keys = []
    for key, value in kwargs.items():
        if value is None:
            keys.append(key)

    for key in keys:
        kwargs.pop(key, None)


def apply_defaults(kwargs, **defaults):
    for key, value in defaults.items():
        if key not in kwargs:
            kwargs[key] = value


def group_as(kwargs, name, values):
    group = {}
    for arg in values:
        if arg in kwargs and kwargs[arg] is not None:
            group[arg] = kwargs.pop(arg, None)
    kwargs[name] = group
