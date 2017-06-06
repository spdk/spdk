from client import print_dict, print_array, int_arg


def set_trace_flag(args):
    params = {'flag': args.flag}
    args.client.call('set_trace_flag', params, verbose=args.verbose)


def clear_trace_flag(args):
    params = {'flag': args.flag}
    args.client.call('clear_trace_flag', params, verbose=args.verbose)


def get_trace_flags(args):
    print_dict(args.client.call('get_trace_flags', verbose=args.verbose))


def set_log_level(args):
    params = {'level': args.level}
    args.client.call('set_log_level', params, verbose=args.verbose)


def get_log_level(args):
    print_dict(args.client.call('get_log_level'), verbose=args.verbose)


def set_log_print_level(args):
    params = {'level': args.level}
    args.client.call('set_log_print_level', params, verbose=args.verbose)


def get_log_print_level(args):
    print_dict(args.client.call('get_log_print_level'), verbose=args.verbose)


def context_switch_monitor(args):
    params = {}
    if args.enable:
        params['enabled'] = True
    if args.disable:
        params['enabled'] = False
    print_dict(args.client.call('context_switch_monitor', params), verbose=args.verbose)
