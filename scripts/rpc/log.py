from client import print_dict, print_array, int_arg


def set_trace_flag(args):
    params = {'flag': args.flag}
    args.client.call('set_trace_flag', params)


def clear_trace_flag(args):
    params = {'flag': args.flag}
    args.client.call('clear_trace_flag', params)


def get_trace_flags(args):
    print_dict(args.client.call('get_trace_flags'))
