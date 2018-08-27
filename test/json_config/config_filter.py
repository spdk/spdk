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
        'set_iscsi_options',
        'set_nvmf_target_config',
        'set_nvmf_target_options',
        'nvmf_create_transport',
        'set_bdev_options',
        'set_bdev_nvme_options',
        'set_bdev_nvme_hotplug',
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


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument('-method', dest='method')

    args = parser.parse_args()
    if args.method == "delete_global_parameters":
        filter_methods(True)
    elif args.method == "delete_configs":
        filter_methods(False)
    elif args.method == "sort":
        """ Wrap input into JSON object so any input is possible here
        like output from get_bdevs RPC method"""
        o = json.loads('{ "the_object": ' + sys.stdin.read() + ' }')
        print(json.dumps(sort_json_object(o)['the_object'], indent=2))
    else:
        raise ValueError("Invalid method '{}'".format(args.method))
