#!/usr/bin/python
import json
import argparse


def filter_methods(filename, remove_pre_init):
    pre_init_methods = [
        'set_iscsi_options',
        'set_nvmf_target_config',
        'set_nvmf_target_options',
        'set_bdev_options'
    ]

    with open(filename) as json_file:
        data = json.loads(json_file.read())
    out = {'subsystems': []}
    for s in data['subsystems']:
        if s['config']:
            s_config = []
            for config in s['config']:
                m_name = config['method']
                is_pre_init = m_name in pre_init_methods
                if remove_pre_init != is_pre_init:
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
    if args.method == "delete_startup_rpcs":
        filter_methods(args.filename, True)
    if args.method == "delete_runtime_rpcs":
        filter_methods(args.filename, False)
