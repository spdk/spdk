#!/usr/bin/env python3

import configparser
import re
import sys
import json
from collections import OrderedDict

bdev_dict = OrderedDict()
bdev_dict["bdev_set_options"] = []
bdev_dict["bdev_split_create"] = []
bdev_dict["bdev_nvme_set_options"] = []
bdev_dict["bdev_nvme_attach_controller"] = []
bdev_dict["bdev_nvme_set_hotplug"] = []
bdev_dict["bdev_malloc_create"] = []
bdev_dict["bdev_aio_create"] = []
bdev_dict["bdev_pmem_create"] = []
bdev_dict["bdev_virtio_attach_controller"] = []

vhost_dict = OrderedDict()
vhost_dict["vhost_create_scsi_controller"] = []
vhost_dict["vhost_create_blk_controller"] = []

iscsi_dict = OrderedDict()
iscsi_dict["iscsi_set_options"] = []
iscsi_dict["iscsi_create_portal_group"] = []
iscsi_dict["iscsi_create_initiator_group"] = []
iscsi_dict["iscsi_create_target_node"] = []

nvmf_dict = OrderedDict()
nvmf_dict["nvmf_set_config"] = []
nvmf_dict["nvmf_set_max_subsystems"] = []
nvmf_dict["subsystems"] = []


# dictionary with new config that will be written to new json config file
subsystem = {
    "accel": None,
    "interface": None,
    "net_framework": None,
    "bdev": bdev_dict,
    "scsi": [],
    "nvmf": nvmf_dict,
    "nbd": [],
    "vhost": vhost_dict,
    "iscsi": iscsi_dict
}


class OptionOrderedDict(OrderedDict):
    def __setitem__(self, option, value):
        if option in self and isinstance(value, list):
            self[option].extend(value)
            return
        super(OptionOrderedDict, self).__setitem__(option, value)


no_yes_map = {"no": False, "No": False, "Yes": True, "yes": True}


def generate_new_json_config():
    json_subsystem = [
        {'subsystem': "accel", 'config': None},
        {"subsystem": "interface", "config": None},
        {"subsystem": "net_framework", "config": None},
        {"subsystem": "bdev", "config": []},
        {"subsystem": "scsi", "config": None},
        {"subsystem": "nvmf", "config": []},
        {"subsystem": "nbd", "config": []},
        {"subsystem": "vhost", "config": []},
        {"subsystem": "iscsi", "config": []}
    ]
    for method in subsystem['bdev']:
        for item in subsystem['bdev'][method]:
            json_subsystem[3]['config'].append(item)
    for item in subsystem['scsi']:
        if json_subsystem[4]['config'] is None:
            json_subsystem[4]['config'] = []
        json_subsystem[4]['config'].append(item)
    for method in subsystem['nvmf']:
        for item in subsystem['nvmf'][method]:
            json_subsystem[5]['config'].append(item)
    for method in subsystem['vhost']:
        for item in subsystem['vhost'][method]:
            json_subsystem[7]['config'].append(item)
    for method in subsystem['iscsi']:
        for item in subsystem['iscsi'][method]:
            json_subsystem[8]['config'].append(item)

    return {"subsystems": json_subsystem}


section_to_subsystem = {
    "Bdev": subsystem['bdev'],
    "AIO": subsystem['bdev'],
    "Malloc": subsystem['bdev'],
    "Nvme": subsystem['bdev'],
    "Pmem": subsystem['bdev'],
    "Split": subsystem['bdev'],
    "Nvmf": subsystem['nvmf'],
    "Subsystem": subsystem['nvmf'],
    "VhostScsi": subsystem['vhost'],
    "VhostBlk": subsystem['vhost'],
    "VhostNvme": subsystem['vhost'],
    "VirtioUser": subsystem['bdev'],
    "iSCSI": subsystem['iscsi'],
    "PortalGroup": subsystem['iscsi'],
    "InitiatorGroup": subsystem['iscsi'],
    "TargetNode": subsystem['iscsi']
}


def set_param(params, cfg_name, value):
    for param in params:
        if param[0] != cfg_name:
            continue
        if param[1] == "disable_chap":
            param[3] = True if value == "None" else False
        elif param[1] == "require_chap":
            param[3] = True if value in ["CHAP", "Mutual"] else False
        elif param[1] == "mutual_chap":
            param[3] = True if value == "Mutual" else False
        elif param[1] == "chap_group":
            param[3] = int(value.replace("AuthGroup", ""))
        elif param[2] == bool:
            param[3] = True if value in ("yes", "true", "Yes") else False
        elif param[2] == "hex":
            param[3] = str(int(value, 16))
        elif param[2] == int:
            param[3] = int(value)
        elif param[2] == list:
            param[3].append(value)
        elif param[2] == "dev_type":
            if value.lower() == "blk":
                param[3] = "blk"
        else:
            param[3] = param[2](value.replace("\"", ""))


def to_json_params(params):
    out = {}
    for param in params:
        if param[3] is not None:
            out[param[1]] = param[3]
    return out


def get_bdev_options_json(config, section):
    params = [
        ["BdevIoPoolSize", "bdev_io_pool_size", int, 65536],
        ["BdevIoCacheSize", "bdev_io_cache_size", int, 256]
    ]
    for option in config.options("Bdev"):
        set_param(params, option, config.get("Bdev", option))

    return [{"params": to_json_params(params), "method": "bdev_set_options"}]


def get_aio_bdev_json(config, section):
    aio_json = []
    value = None
    for option in config.options("AIO"):
        if option == "AIO":
            value = config.get("AIO", option).split("\n")
    if value is None:
        return aio_json
    for item in value:
        items = re.findall(r"\S+", item)
        params = {}
        params['filename'] = items[0]
        params['name'] = items[1]
        if len(items) == 3:
            params['block_size'] = int(items[2])
        aio_json.append({
            "params": params,
            "method": "bdev_aio_create"
        })

    return aio_json


def get_malloc_bdev_json(config, section):
    malloc_json = []
    params = [
        ['NumberOfLuns', '', int, -1],
        ['LunSizeInMB', '', int, 20],
        ['BlockSize', '', int, 512]
    ]
    for option in config.options("Malloc"):
        set_param(params, option, config.get("Malloc", option))
    for lun in range(0, params[0][3]):
        malloc_json.append({
            "params": {
                "block_size": params[2][3],
                "num_blocks": params[1][3] * 1024 * 1024 / params[2][3],
                "name": "Malloc%s" % lun
            },
            "method": "bdev_malloc_create"
        })

    return malloc_json


def get_nvme_bdev_json(config, section):
    params = [
        ["RetryCount", "retry_count", int, 4],
        ["TimeoutuSec", "timeout_us", int, 0],
        ["AdminPollRate", "nvme_adminq_poll_period_us", int, 1000000],
        ["ActionOnTimeout", "action_on_timeout", str, "none"],
        ["IOPollRate", "nvme_ioq_poll_period_us", int, 0],
        ["HotplugEnable", "enable", bool, False],
        ["AdminPollRate", "period_us", int, 1000]
    ]
    nvme_json = []
    for option in config.options("Nvme"):
        value = config.get("Nvme", option)
        if "TransportID" == option:
            entry = re.findall(r"\S+", value)
            nvme_name = entry[-1]
            trtype = re.findall(r"trtype:\S+", value)
            if trtype:
                trtype = trtype[0].replace("trtype:", "").replace("\"", "")
            traddr = re.findall(r"traddr:\S+", value)
            if traddr:
                traddr = traddr[0].replace("traddr:", "").replace("\"", "")
            nvme_json.append({
                "params": {
                    "trtype": trtype,
                    "name": nvme_name,
                    "traddr": traddr
                },
                "method": "bdev_nvme_attach_controller"
            })
        else:
            set_param(params, option, value)
    params[3][3] = params[3][3].lower()
    params[6][3] = params[6][3] * 100
    nvme_json.append({
        "params": to_json_params(params[5:7]),
        "method": "bdev_nvme_set_hotplug"
    })
    nvme_json.append({
        "params": to_json_params(params[0:5]),
        "method": "bdev_nvme_set_options"
    })
    return nvme_json


def get_pmem_bdev_json(config, section):
    pmem_json = []
    for option in config.options(section):
        if "Blk" == option:
            for value in config.get(section, option).split("\n"):
                items = re.findall(r"\S+", value)
                pmem_json.append({
                    "params": {
                        "name": items[1],
                        "pmem_file": items[0]
                    },
                    "method": "bdev_pmem_create"
                })

    return pmem_json


def get_split_bdev_json(config, section):
    split_json = []
    value = []
    for option in config.options("Split"):
        if "Split" == option:
            value = config.get("Split", option)
    if value and not isinstance(value, list):
        value = [value]
    for split in value:
        items = re.findall(r"\S+", split)
        split_size_mb = 0
        base_bdev = items[0]
        split_count = int(items[1])
        if len(items) == 3:
            split_size_mb = items[2]
        split_json.append({
            "params": {
                "base_bdev": base_bdev,
                "split_size_mb": split_size_mb,
                "split_count": split_count
            },
            "method": "bdev_split_create"
        })

    return split_json


def get_nvmf_options_json(config, section):
    params = [
        ["AcceptorPollRate", "acceptor_poll_rate", int, 10000],
        ["MaxSubsystems", "max_subsystems", int, 1024]
    ]
    for option in config.options("Nvmf"):
        set_param(params, option, config.get("Nvmf", option))
    nvmf_json = []
    nvmf_json.append({
        "params": to_json_params([params[0]]),
        "method": "nvmf_set_config"
    })
    nvmf_json.append({
        "params": to_json_params(params[1:7]),
        "method": "nvmf_set_max_subsystems"
    })

    return nvmf_json


def get_nvmf_subsystem_json(config, section):
    nvmf_subsystem_methods = []
    params = [
        # Last items are default values if given entry is not set
        ["Host", "hosts", list, []],
        ["NQN", "nqn", str, ""],
        ["AllowAnyHost", "allow_any_host", bool, False],
        ["SN", "serial_number", str, "00000000000000000000"],
        ["MN", "model_number", str, "SPDK bdev Controller"],
        ["MaxNamespaces", "max_namespaces", str, ""],
    ]
    listen_address = []
    namespaces = []
    nsid = 0
    searched_items = [param[0] for param in params]
    for option in config.options(section):
        value = config.get(section, option)
        if option in searched_items:
            set_param(params, option, value)
            continue
        if "Listen" == option:
            items = re.findall(r"\S+", value)
            adrfam = "IPv4"
            if len(items[1].split(":")) > 2:
                adrfam = "IPv6"
            listen_address.append({
                "trtype": items[0],
                "adrfam": adrfam,
                "trsvcid": items[1].rsplit(":", 1)[-1],
                "traddr": items[1].rsplit(":", 1)[0].replace(
                    "]", "").replace("[", "")
            })
        if "Namespace" == option:
            for item in value.split("\n"):
                items = re.findall(r"\S+", item)
                if len(items) == 2:
                    nsid = items[1]
                else:
                    nsid += 1
                namespaces.append({
                    "nsid": int(nsid),
                    "bdev_name": items[0],
                })
    # Get parameters: nqn, allow_any_host, serial_number
    # for nvmf_create_subsystem rpc method
    parameters = to_json_params(params[1:5])
    nvmf_subsystem_methods.append({
        "params": parameters,
        "method": "nvmf_create_subsystem"
    })
    for listen in listen_address:
        nvmf_subsystem_methods.append({
            "params": {
                "listen_address": listen,
                "nqn": parameters['nqn']
            },
            "method": "nvmf_subsystem_add_listener"
        })
    for host in to_json_params([params[0]])['hosts']:
        nvmf_subsystem_methods.append({
            "params": {
                "host": host,
                "nqn": parameters['nqn']
            },
            "method": "nvmf_subsystem_add_host"
        })
    for namespace in namespaces:
        nvmf_subsystem_methods.append({
            "params": {
                "namespace": namespace,
                "nqn": parameters['nqn']
            },
            "method": "nvmf_subsystem_add_ns"
        })

    # Define max_namespaces if it is set in old config
    if params[5][3]:
        nvmf_subsystem_methods[0]['params']['max_namespaces'] = int(params[5][3])

    return nvmf_subsystem_methods


def get_vhost_scsi_json(config, section):
    params = [
        ["Name", "ctrlr", str, None],
        ["Cpumask", "cpumask", "hex", "1"],
    ]
    targets = []
    vhost_scsi_json = []
    for option in config.options(section):
        value = config.get(section, option)
        if option in ["Name", "Cpumask"]:
            set_param(params, option, value)
        if "Target" == option:
            for item in value.split("\n"):
                items = re.findall(r"\S+", item)
                targets.append({
                    "scsi_target_num": int(items[0]),
                    "ctrlr": params[0][3],
                    "bdev_name": items[1]
                })
    vhost_scsi_json.append({
        "params": to_json_params(params),
        "method": "vhost_create_scsi_controller"
    })
    for target in targets:
        vhost_scsi_json.append({
            "params": target,
            "method": "vhost_scsi_controller_add_target"
        })

    return vhost_scsi_json


def get_vhost_blk_json(config, section):
    params = [
        ["ReadOnly", "readonly", bool, False],
        ["Dev", "dev_name", str, ""],
        ["Name", "ctrlr", str, ""],
        ["Cpumask", "cpumask", "hex", ""]
    ]
    for option in config.options(section):
        set_param(params, option, config.get(section, option))
    return [{"method": "vhost_create_blk_controller",
            "params": to_json_params(params)}]


def get_virtio_user_json(config, section):
    params = [
        ["Path", "traddr", str, ""],
        ["Queues", "vq_count", int, 1],
        ["Type", "dev_type", "dev_type", "scsi"],
        ["Name", "name", str, section],
        # Define parameters with default values.
        # These params are set by rpc commands and
        # do not occur in ini config file.
        # But they are visible in json config file
        # with default values even if not set by rpc.
        [None, "trtype", str, "user"],
        [None, "vq_size", int, 512]
    ]
    for option in config.options(section):
        set_param(params, option, config.get(section, option))
    dev_name = "Scsi"
    if params[2][3] == "blk":
        dev_name = "Blk"
    params[3][3] = params[3][3].replace("User", dev_name)

    return [{
        "params": to_json_params(params),
        "method": "bdev_virtio_attach_controller"
    }]


def get_iscsi_options_json(config, section):
    params = [
        ['AllowDuplicateIsid', 'allow_duplicated_isid', bool, False],
        ['DefaultTime2Retain', 'default_time2retain', int, 20],
        ['DiscoveryAuthMethod', 'mutual_chap', bool, False],
        ['MaxConnectionsPerSession', 'max_connections_per_session', int, 2],
        ['Timeout', 'nop_timeout', int, 60],
        ['DiscoveryAuthMethod', 'disable_chap', bool, False],
        ['DiscoveryAuthMethod', 'require_chap', bool, False],
        ['NodeBase', 'node_base', str, "iqn.2016-06.io.spdk"],
        ['AuthFile', 'auth_file', str, None],
        ['DiscoveryAuthGroup', 'chap_group', int, 0],
        ['MaxSessions', 'max_sessions', int, 128],
        ['ImmediateData', 'immediate_data', bool, True],
        ['ErrorRecoveryLevel', 'error_recovery_level', int, 0],
        ['NopInInterval', 'nop_in_interval', int, 30],
        ['DefaultTime2Wait', 'default_time2wait', int, 2],
        ['QueueDepth', 'max_queue_depth', int, 64],
        ['', 'first_burst_length', int, 8192]
    ]
    for option in config.options(section):
        set_param(params, option, config.get(section, option))
    return [{"method": "iscsi_set_options", "params": to_json_params(params)}]


def get_iscsi_portal_group_json(config, name):
    portal_group_json = []
    portals = []
    for option in config.options(name):
        if "Portal" == option:
            for value in config.get(name, option).split("\n"):
                items = re.findall(r"\S+", value)
                portal = {'host': items[1].rsplit(":", 1)[0]}
                if "@" in items[1]:
                    portal['port'] =\
                        items[1].rsplit(":", 1)[1].split("@")[0]
                else:
                    portal['port'] = items[1].rsplit(":", 1)[1]
                portals.append(portal)

    portal_group_json.append({
        "params": {
            "portals": portals,
            "tag": int(re.findall(r'\d+', name)[0])
        },
        "method": "iscsi_create_portal_group"
    })

    return portal_group_json


def get_iscsi_initiator_group_json(config, name):
    initiators = []
    netmasks = []

    for option in config.options(name):
        if "InitiatorName" == option:
            initiators.append(config.get(name, option))
        if "Netmask" == option:
            netmasks.append(config.get(name, option))
    initiator_group_json = {
        "params": {
            "initiators": initiators,
            "tag": int(re.findall(r'\d+', name)[0]),
            "netmasks": netmasks
        },
        "method": "iscsi_create_initiator_group"
    }

    return [initiator_group_json]


def get_iscsi_target_node_json(config, section):
    luns = []
    mutual_chap = False
    name = ""
    alias_name = ""
    require_chap = False
    chap_group = 1
    pg_ig_maps = []
    data_digest = False
    disable_chap = False
    header_digest = False
    queue_depth = 64

    for option in config.options(section):
        value = config.get(section, option)
        if "TargetName" == option:
            name = value
        if "TargetAlias" == option:
            alias_name = value.replace("\"", "")
        if "Mapping" == option:
            items = re.findall(r"\S+", value)
            pg_ig_maps.append({
                "ig_tag": int(re.findall(r'\d+', items[1])[0]),
                "pg_tag": int(re.findall(r'\d+', items[0])[0])
            })
        if "AuthMethod" == option:
            items = re.findall(r"\S+", value)
            for item in items:
                if "CHAP" == item:
                    require_chap = True
                elif "Mutual" == item:
                    mutual_chap = True
                elif "Auto" == item:
                    disable_chap = False
                    require_chap = False
                    mutual_chap = False
                elif "None" == item:
                    disable_chap = True
                    require_chap = False
                    mutual_chap = False
        if "AuthGroup" == option:  # AuthGroup1
            items = re.findall(r"\S+", value)
            chap_group = int(re.findall(r'\d+', items[0])[0])
        if "UseDigest" == option:
            items = re.findall(r"\S+", value)
            for item in items:
                if "Header" == item:
                    header_digest = True
                elif "Data" == item:
                    data_digest = True
                elif "Auto" == item:
                    header_digest = False
                    data_digest = False

        if re.match(r"LUN\d+", option):
            luns.append({"lun_id": len(luns),
                         "bdev_name": value})
        if "QueueDepth" == option:
            queue_depth = int(value)

    params = {"alias_name": alias_name}
    params["name"] = "iqn.2016-06.io.spdk:%s" % name
    params["luns"] = luns
    params["pg_ig_maps"] = pg_ig_maps
    params["queue_depth"] = queue_depth
    params["chap_group"] = chap_group
    params["header_digest"] = header_digest
    params["mutual_chap"] = mutual_chap
    params["require_chap"] = require_chap
    params["data_digest"] = data_digest
    params["disable_chap"] = disable_chap

    target_json = {
        "params": params,
        "method": "iscsi_create_target_node"
    }

    return [target_json]


if __name__ == "__main__":
    try:
        config = configparser.ConfigParser(strict=False, delimiters=(' '),
                                           dict_type=OptionOrderedDict,
                                           allow_no_value=True)
        # Do not parse options and values. Capital letters are relevant.
        config.optionxform = str
        config.read_file(sys.stdin)
    except Exception as e:
        print("Exception while parsing config: %s" % e)
        exit(1)
    # Add missing sections to generate default configuration
    for section in ['Nvme', 'Nvmf', 'Bdev', 'iSCSI']:
        if section not in config.sections():
            config.add_section(section)

    for section in config.sections():
        match = re.match(r'(Bdev|Nvme|Malloc|VirtioUser\d+|Split|Pmem|AIO|'
                         r'iSCSI|PortalGroup\d+|InitiatorGroup\d+|'
                         r'TargetNode\d+|Nvmf|Subsystem\d+|VhostScsi\d+|'
                         r'VhostBlk\d+)', section)
        if match:
            match_section = ''.join(letter for letter in match.group(0)
                                    if not letter.isdigit())
            if match_section == "Bdev":
                items = get_bdev_options_json(config, section)
            elif match_section == "AIO":
                items = get_aio_bdev_json(config, section)
            elif match_section == "Malloc":
                items = get_malloc_bdev_json(config, section)
            elif match_section == "Nvme":
                items = get_nvme_bdev_json(config, section)
            elif match_section == "Pmem":
                items = get_pmem_bdev_json(config, section)
            elif match_section == "Split":
                items = get_split_bdev_json(config, section)
            elif match_section == "Nvmf":
                items = get_nvmf_options_json(config, section)
            elif match_section == "Subsystem":
                items = get_nvmf_subsystem_json(config, section)
            elif match_section == "VhostScsi":
                items = get_vhost_scsi_json(config, section)
            elif match_section == "VhostBlk":
                items = get_vhost_blk_json(config, section)
            elif match_section == "VirtioUser":
                items = get_virtio_user_json(config, section)
            elif match_section == "iSCSI":
                items = get_iscsi_options_json(config, section)
            elif match_section == "PortalGroup":
                items = get_iscsi_portal_group_json(config, section)
            elif match_section == "InitiatorGroup":
                items = get_iscsi_initiator_group_json(config, section)
            elif match_section == "TargetNode":
                items = get_iscsi_target_node_json(config, section)
            for item in items:
                if match_section == "VhostScsi":
                    section_to_subsystem[match_section]["vhost_create_scsi_controller"].append(item)
                elif match_section == "Subsystem":
                    section_to_subsystem[match_section]["subsystems"].append(item)
                else:
                    section_to_subsystem[match_section][
                        item['method']].append(item)
        elif section == "Global":
            pass
        elif section == "Ioat":
            # Ioat doesn't support JSON config yet.
            pass
        elif section == "VirtioPci":
            print("Please use spdk target flags.")
            exit(1)
        else:
            print("An invalid section detected: %s.\n"
                  "Please revise your config file." % section)
            exit(1)
    json.dump(generate_new_json_config(), sys.stdout, indent=2)
    print("")
