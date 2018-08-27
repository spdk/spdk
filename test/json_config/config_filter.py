#!/usr/bin/python
import json
import argparse


def filter_methods(filename, do_remove_global_rpcs):
    global_rpcs = [
        'set_iscsi_options',
        'set_nvmf_target_config',
        'set_nvmf_target_options',
        'nvmf_create_transport',
        'set_bdev_options',
        'set_bdev_nvme_options',
        'set_bdev_nvme_hotplug',
    ]

    with open(filename) as json_file:
        data = json.loads(json_file.read())
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

    print json.dumps(out, indent=2)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument('-method', dest='method')
    parser.add_argument('-filename', dest='filename')

    args = parser.parse_args()
    if args.method == "delete_global_parameters":
        filter_methods(args.filename, True)
    if args.method == "delete_configs":
        filter_methods(args.filename, False)
