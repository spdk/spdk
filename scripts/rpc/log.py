def set_trace_flag(client, args):
    params = {'flag': args.flag}
    return client.call('set_trace_flag', params)


def clear_trace_flag(client, args):
    params = {'flag': args.flag}
    return client.call('clear_trace_flag', params)


def get_trace_flags(client, args):
    return client.call('get_trace_flags')


def set_log_level(client, args):
    params = {'level': args.level}
    return client.call('set_log_level', params)


def get_log_level(client, args):
    return client.call('get_log_level')


def set_log_print_level(client, args):
    params = {'level': args.level}
    return client.call('set_log_print_level', params)


def get_log_print_level(client, args):
    return client.call('get_log_print_level')
