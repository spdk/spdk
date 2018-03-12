def set_trace_flag(args):
    params = {'flag': args.flag}
    return args.client.call('set_trace_flag', params)


def clear_trace_flag(args):
    params = {'flag': args.flag}
    return args.client.call('clear_trace_flag', params)


def get_trace_flags(args):
    return args.client.call('get_trace_flags')


def set_log_level(args):
    params = {'level': args.level}
    return args.client.call('set_log_level', params)


def get_log_level(args):
    return args.client.call('get_log_level')


def set_log_print_level(args):
    params = {'level': args.level}
    return args.client.call('set_log_print_level', params)


def get_log_print_level(args):
    return args.client.call('get_log_print_level')
