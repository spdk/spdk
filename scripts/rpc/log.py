from client import print_dict, print_array, int_arg


def set_trace_flag(args):
    params = {'flag': args.flag}
    args.client.call('set_trace_flag', params)


def clear_trace_flag(args):
    params = {'flag': args.flag}
    args.client.call('clear_trace_flag', params)


def get_trace_flags(args):
    print_dict(args.client.call('get_trace_flags'))


def set_log_level(args):
    params = {'level': args.level}
    args.client.call('set_log_level', params)


def get_log_level(args):
    print_dict(args.client.call('get_log_level'))


def set_log_print_level(args):
    params = {'level': args.level}
    args.client.call('set_log_print_level', params)


def get_log_print_level(args):
    print_dict(args.client.call('get_log_print_level'))
