from client import print_dict, print_array, int_arg


def hist_enable(args):
    params = {
        'hist_id': args.hist_id
    }
    print(args.client.call('hist_enable', params))

def hist_get_stats(args):
    params = {
        'hist_id': args.hist_id
    }
    print_dict(args.client.call('hist_get_stats', params))

def hist_list_ids(args):
    print_dict(args.client.call('hist_list_ids'))

def hist_disable(args):
    params = {
        'hist_id': args.hist_id
    }
    print(args.client.call('hist_disable', params))

def hist_clear(args):
    params = {
        'hist_id': args.hist_id
    }
    print(args.client.call('hist_clear', params))

def hist_clear_all(args):
    print(args.client.call('hist_clear_all'))
