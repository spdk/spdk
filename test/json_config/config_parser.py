#!/usr/bin/python
import json
import argparse


def delete_unneeded_params(config, method_list, in_list=True):
    to_delete = []
    for i, value in enumerate(config):
        if in_list:
            if value['method'] in method_list:
                to_delete.append(value)
        else:
            if value['method'] not in method_list:
                to_delete.append(value)

    return to_delete


# Delete method from spdk target config that are loading in first iteration
def skip_params(filename):
    with open(filename) as json_file:
        data = json.loads(json_file.read())
    for subsystem in data['subsystems']:
        if subsystem['subsystem'] == "iscsi":
            for config_method in reversed(delete_unneeded_params(subsystem['config'],
                                                                 ["set_iscsi_options"])):
                subsystem['config'].remove(config_method)
        if subsystem['subsystem'] == "nvmf":
            for config_method in reversed(delete_unneeded_params(
                    subsystem['config'], ["set_nvmf_target_config", "set_nvmf_target_options"])):
                subsystem['config'].remove(config_method)
    print json.dumps(data, indent=2)


# Delete method from spdk target config that are loading in second iteration
def get_params(filename):
    with open(filename) as json_file:
        data = json.loads(json_file.read())
    for subsystem in data['subsystems']:
        if subsystem['subsystem'] == "iscsi":
            for config_method in reversed(delete_unneeded_params(subsystem['config'],
                                                                 "set_iscsi_options", False)):
                subsystem['config'].remove(config_method)
        elif subsystem['subsystem'] == "nvmf":
            for config_method in reversed(delete_unneeded_params(
                    subsystem['config'], ["set_nvmf_target_config", "set_nvmf_target_options"], False)):
                subsystem['config'].remove(config_method)
        else:
            del subsystem['subsystem']
    print json.dumps(data, indent=2)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument('-method', dest='method')
    parser.add_argument('-filename', dest='filename')

    args = parser.parse_args()
    if args.method == "skip_params":
        skip_params(args.filename)
    if args.method == "get_params":
        get_params(args.filename)
