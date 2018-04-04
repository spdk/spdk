#!/usr/bin/python
import json
import argparse


def delete_unneeded_params(config, method_list, in_list=True):
    to_delete = []
    for i, value in enumerate(config):
        if in_list:
            if config[i]['method'] in method_list:
                to_delete.append(i)
        else:
            if config[i]['method'] not in method_list:
                to_delete.append(i)

    return to_delete


# Delete method from spdk target config that are loading in first iteration
def skip_params(filename):
    with open(filename) as json_file:
        data = json.loads(json_file.read())
    for subsystem in data['subsystems']:
        if subsystem['subsystem'] == "iscsi":
            for i in reversed(delete_unneeded_params(subsystem['config'], ["set_iscsi_options"])):
                del subsystem['config'][i]
        if subsystem['subsystem'] == "nvmf":
            for i in reversed(delete_unneeded_params(
                    subsystem['config'], ["set_nvmf_target_config", "set_nvmf_target_options"])):
                del subsystem['config'][i]
    print json.dumps(data, indent=2)


# Delete method from spdk target config that are loading in second iteration
def get_params(filename):
    with open(filename) as json_file:
        data = json.loads(json_file.read())
    for subsystem in data['subsystems']:
        if subsystem['subsystem'] == "iscsi":
            for i in reversed(delete_unneeded_params(subsystem['config'],
                                                     "set_iscsi_options", False)):
                del subsystem['config'][i]
        elif subsystem['subsystem'] == "nvmf":
            for i in reversed(delete_unneeded_params(
                    subsystem['config'], ["set_nvmf_target_config", "set_nvmf_target_options"], False)):
                del subsystem['config'][i]
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
