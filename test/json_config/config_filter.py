#!/usr/bin/env python3

import sys
import json
import argparse
from collections import OrderedDict


def sort_json_object(o):
    if isinstance(o, dict):
        sorted_o = OrderedDict()
        """ Order of keys in JSON object is irrelevant but we need to pick one
        to be able to compare JSONS. """
        for key in sorted(o.keys()):
            sorted_o[key] = sort_json_object(o[key])
        return sorted_o
    if isinstance(o, list):
        """ Keep list in the same orded but sort each item """
        return [sort_json_object(item) for item in o]
    else:
        return o


def filter_methods(do_remove_global_rpcs):
    global_rpcs = [
        'idxd_scan_accel_engine',
        'iscsi_set_options',
        'nvmf_set_config',
        'nvmf_set_max_subsystems',
        'nvmf_create_transport',
        'bdev_set_options',
        'bdev_nvme_set_options',
        'bdev_nvme_set_hotplug',
        'sock_impl_set_options',
        'sock_set_default_impl',
    ]

    data = json.loads(sys.stdin.read())
    out = {'subsystems': []}
    for s in data['subsystems']:
        if s['config']:
            s_config = []
            for config in s['config']:
                m_name = config['method']
                is_global_rpc = m_name in global_rpcs
                if do_remove_global_rpcs != is_global_rpc:
                    s_config.append(config)
        else:
            s_config = None
        out['subsystems'].append({
            'subsystem': s['subsystem'],
            'config': s_config,
        })

    print(json.dumps(out, indent=2))


def check_empty():
    data = json.loads(sys.stdin.read())
    if not data:
        raise EOFError("Cant read config!")

    for s in data['subsystems']:
        if s['config']:
            print("Config not empty")
            print(s['config'])
            sys.exit(1)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(formatter_class=argparse.RawTextHelpFormatter)
    parser.add_argument('-method', dest='method', default=None,
                        help="""One of the methods:
check_empty
    check if provided configuration is logically empty
delete_global_parameters
    remove pre-init configuration (pre framework_start_init RPC methods)
delete_configs
    remove post-init configuration (post framework_start_init RPC methods)
sort
    remove nothing - just sort JSON objects (and subobjects but not arrays)
    in lexicographical order. This can be used to do plain text diff.""")

    args = parser.parse_args()
    if args.method == "delete_global_parameters":
        filter_methods(True)
    elif args.method == "delete_configs":
        filter_methods(False)
    elif args.method == "check_empty":
        check_empty()
    elif args.method == "sort":
        """ Wrap input into JSON object so any input is possible here
        like output from bdev_get_bdevs RPC method"""
        o = json.loads('{ "the_object": ' + sys.stdin.read() + ' }')
        print(json.dumps(sort_json_object(o)['the_object'], indent=2))
    else:
        raise ValueError("Invalid method '{}'\n\n{}".format(args.method, parser.format_help()))
