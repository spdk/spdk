#!/usr/bin/python
import json
import argparse


def skip_params(filename):
    with open(filename) as json_file:
        data = json.loads(json_file.read())
        for subsystem in data['subsystems']:
            if subsystem['subsystem'] == "iscsi":
                to_delete = []
                for config_i in range(0, len(subsystem['config'])):
                    if subsystem['config'][config_i]['method'] == "set_iscsi_options":
                        to_delete.append(config_i)
                for i in reversed(to_delete):
                    del subsystem['config'][i]
            if subsystem['subsystem'] == "nvmf":
                to_delete = []
                for config_i in range(0, len(subsystem['config'])):
                    if subsystem['config'][config_i]['method'] == "set_nvmf_target_config":
                        to_delete.append(config_i)
                    if subsystem['config'][config_i]['method'] == "set_nvmf_target_options":
                        to_delete.append(config_i)
                for i in reversed(to_delete):
                    del subsystem['config'][i]
    print json.dumps(data, indent=2)


def get_params(filename):
    with open(filename) as json_file:
        data = json.loads(json_file.read())
        for subsystem in data['subsystems']:
            if subsystem['subsystem'] == "iscsi":
                to_delete = []
                for config_i in range(0, len(subsystem['config'])):
                    if subsystem['config'][config_i]['method'] != "set_iscsi_options":
                        to_delete.append(config_i)
                for i in reversed(to_delete):
                    del subsystem['config'][i]
            elif subsystem['subsystem'] == "nvmf":
                to_delete = []
                for config_i in range(0, len(subsystem['config'])):
                    if subsystem['config'][config_i]['method'] not in ["set_nvmf_target_config",
                                                                       "set_nvmf_target_options"]:
                        to_delete.append(config_i)
                for i in reversed(to_delete):
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
