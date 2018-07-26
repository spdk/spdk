#!/usr/bin/env python

import configparser
import argparse
import re
import json
from collections import OrderedDict

portal_groups_map = {}
initiator_groups_map = {}
global pg_tag_id
global ig_tag_id
pg_tag_id = 0
ig_tag_id = 0


# dictionary with new config that will be written to new json config file
new_json_config = {
    "subsystems": [
        {
            "subsystem": "copy",
            "config": None
        },
        {
            "subsystem": "interface",
            "config": None
        },
        {
            "subsystem": "net_framework",
            "config": None
        },
        {
            "subsystem": "bdev",
            "config": []
        },
        {
            "subsystem": "scsi",
            "config": None
        },
        {
            "subsystem": "nvmf",
            "config": []
        },
        {
            "subsystem": "nbd",
            "config": []
        },
        {
            "subsystem": "vhost",
            "config": []
        },
        {
            "subsystem": "iscsi",
            "config": []
        }
    ]
}


class OptionOrderedDict(OrderedDict):
    def __setitem__(self, option, value):
        if option in self and isinstance(value, list):
            self[option].extend(value)
            return
        super(OptionOrderedDict, self).__setitem__(option, value)


def change_subsystem_config_type(subsystem):
    if subsystem['config'] is None:
        subsystem['config'] = []


bdev_method_order = {
    "set_bdev_options": 0,
    "construct_split_vbdev": 1,
    "set_bdev_nvme_options": 2,
    "construct_nvme_bdev": 3,
    "set_bdev_nvme_hotplug": 4,
    "construct_malloc_bdev": 5,
    "construct_aio_bdev": 6,
    "construct_pmem_bdev": 7,
    "construct_virtio_dev": 8
}


def get_bdev_insert_index(method):
    insert_index = bdev_method_order[method]
    bdev_subsystem = []
    for subsystem in new_json_config["subsystems"]:
        if subsystem['subsystem'] == "bdev":
            bdev_subsystem = subsystem['config']

    i = 0
    for item in bdev_subsystem:
        item_index = bdev_method_order[item['method']]
        if insert_index < item_index:
            break
        i += 1

    return i


vhost_method_order = {
    "construct_vhost_scsi_controller": 0,
    "construct_vhost_blk_controller": 1,
    "construct_vhost_nvme_controller": 2
}


def get_vhost_insert_index(method):
    insert_index = vhost_method_order[method]
    vhost_subsystem = []
    for subsystem in new_json_config["subsystems"]:
        if subsystem['subsystem'] == "vhost":
            vhost_subsystem = subsystem['config']

    i = 0
    index_found = False
    for item in vhost_subsystem:
        if item['method'] not in vhost_method_order:
            i += 1
            continue
        item_index = vhost_method_order[item['method']]
        if insert_index < item_index:
            break
        i += 1

    return i


iscsi_method_order = {
    "set_iscsi_options": 0,
    "add_portal_group": 1,
    "add_initiator_group": 2,
    "construct_target_node": 3
}


def get_iscsi_insert_index(method):
    insert_index = iscsi_method_order[method]
    iscsi_subsystem = []
    for subsystem in new_json_config["subsystems"]:
        if subsystem['subsystem'] == "iscsi":
            iscsi_subsystem = subsystem['config']

    i = 0
    for item in iscsi_subsystem:
        item_index = iscsi_method_order[item['method']]
        if insert_index < item_index:
            break
        i += 1

    return i


def get_subsystem(subsystem_name):
    for subsystem in new_json_config['subsystems']:
        if subsystem['subsystem'] == subsystem_name:
            change_subsystem_config_type(subsystem)
            return subsystem

    return None


no_yes_map = {"no": False, "No": False, "Yes": True, "yes": True}


def parse_old_config(old_config, new_config):
    try:
        config = configparser.ConfigParser(strict=False, delimiters=(' '),
                                           dict_type=OptionOrderedDict,
                                           allow_no_value=True)
        config.optionxform = str
        config.read(old_config)
    except Exception as e:
        print("Exception while parsing config: %s" % e)
        return
    # Add missing sections
    for section in ['Nvme', 'Nvmf', 'Bdev', 'iSCSI']:
        if section not in config.sections():
            config.add_section(section)

    for section in config.sections():
        subsystem = get_subsystem
        if section == "Nvme":
            nvme_json = get_nvme_json(config)
            subsystem = get_subsystem("bdev")
            for item in nvme_json:
                index = get_bdev_insert_index(item['method'])
                subsystem['config'].insert(index, item)
        elif section == "Malloc":
            malloc_json = get_malloc_json(config)
            subsystem = get_subsystem("bdev")
            for item in malloc_json:
                index = get_bdev_insert_index(item['method'])
                subsystem['config'].insert(index, item)
        elif re.match("VirtioUser\d+", section):
            virtio_json = get_virtio_user_json(config, section)
            subsystem = get_subsystem("bdev")
            index = get_bdev_insert_index(virtio_json['method'])
            subsystem['config'].insert(index, virtio_json)
        elif section == "Split":
            split_json = get_split_json(config)
            for split in split_json:
                subsystem = get_subsystem("bdev")
                index = get_bdev_insert_index(split['method'])
                subsystem['config'].insert(index, split)
        elif section == "iSCSI":
            iscsi_json = get_iscsi_json(config, section)
            subsystem = get_subsystem("iscsi")
            index = get_iscsi_insert_index(iscsi_json['method'])
            subsystem['config'].insert(index, iscsi_json)
        elif section == "Pmem":
            pmem_json = get_pmem_json(config, section)
            subsystem = get_subsystem("bdev")
            for item in pmem_json:
                index = get_bdev_insert_index(item['method'])
                subsystem['config'].insert(index, item)
        elif section == "AIO":
            aio_json = get_aio_json(config)
            subsystem = get_subsystem("bdev")
            for item in aio_json:
                index = get_bdev_insert_index(item['method'])
                subsystem['config'].insert(index, item)
        elif section == "Bdev":
            bdev_json = get_bdev_json(config)
            subsystem = get_subsystem("bdev")
            index = get_bdev_insert_index(bdev_json['method'])
            subsystem['config'].insert(index, bdev_json)
        elif re.match("PortalGroup\d+", section):
            portal_group_json = get_portal_group_json(config, section)
            subsystem = get_subsystem("iscsi")
            for item in portal_group_json:
                index = get_iscsi_insert_index(item['method'])
                subsystem['config'].insert(index, item)
        elif re.match("InitiatorGroup\d+", section):
            initiator_group_json = get_initiator_group_json(config, section)
            subsystem = get_subsystem("iscsi")
            index = get_iscsi_insert_index(initiator_group_json['method'])
            subsystem['config'].insert(index, initiator_group_json)
        elif re.match("TargetNode\d+", section):
            target_node_json = get_target_node_json(config, section)
            subsystem = get_subsystem("iscsi")
            index = get_iscsi_insert_index(target_node_json['method'])
            subsystem['config'].insert(index, target_node_json)
        elif re.match("Nvmf", section):
            nvmf_json = get_nvmf_json(config)
            subsystem = get_subsystem("nvmf")
            for item in nvmf_json:
                subsystem['config'].append(item)
        elif re.match("Subsystem\d+", section):
            subsystem_json = get_subsystem_json(config, section)
            subsystem = get_subsystem("nvmf")
            subsystem['config'].append(subsystem_json)
        elif re.match("VhostScsi\d+", section):
            vhost_scsi_json = get_vhost_scsi_json(config, section)
            subsystem = get_subsystem("vhost")
            index = get_vhost_insert_index(vhost_scsi_json[0]['method'])
            for item in vhost_scsi_json:
                subsystem['config'].insert(index, item)
                index += 1
        elif re.match("VhostBlk\d+", section):
            vhost_blk_json = get_vhost_blk_json(config, section)
            subsystem = get_subsystem("vhost")
            index = get_vhost_insert_index(vhost_blk_json['method'])
            subsystem['config'].insert(index, vhost_blk_json)
        elif re.match("VhostNvme\d+", section):
            vhost_nvme_json = get_vhost_nvme_json(config, section)
            subsystem = get_subsystem("vhost")
            index = get_vhost_insert_index(vhost_nvme_json[0]['method'])
            for item in vhost_nvme_json:
                subsystem['config'].insert(index, item)
                index += 1
        else:
            print("An invalid section detected: %s.\n"
                  "Please revise your config file." % section)
    with open(new_config, "w") as new_file:
        json.dump(new_json_config, new_file, indent=2)
        new_file.write("\n")


def set_param(params, cfg_name, value):
    for param in params:
        if param[0] != cfg_name:
            continue
        if param[1] == "no_discovery_auth":
            param[3] = True if value == "None" else False
        elif param[1] == "req_discovery_auth":
            param[3] = True if value in ["CHAP", "Mutual"] else False
        elif param[1] == "req_discovery_auth_mutual":
            param[3] = True if value == "Mutual" else False
        elif param[1] == "discovery_auth_group":
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
        out[param[1]] = param[3]
    return out


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
                items = re.findall("\S+", item)
                targets.append({
                    "scsi_target_num": int(items[0]),
                    "ctrlr": params[0][3],
                    "bdev_name": items[1]
                })
    vhost_scsi_json.append({
        "params": to_json_params(params),
        "method": "construct_vhost_scsi_controller"
    })
    for target in targets:
        vhost_scsi_json.append({
            "params": target,
            "method": "add_vhost_scsi_lun"
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
    return {"method": "construct_vhost_blk_controller",
            "params": to_json_params(params)}


def get_vhost_nvme_json(config, section):
    params = [
        ["Name", "ctrlr", str, ""],
        ["NumberOfQueues", "io_queues", int, -1],
        ["Cpumask", "cpumask", "hex", 0x1],
        ["Namespace", "bdev_name", list, []]
    ]
    for option in config.options(section):
        values = config.get(section, option).split("\n")
        for value in values:
            set_param(params, option, value)
    vhost_nvme_json = []
    vhost_nvme_json.append({
        "params": to_json_params(params[:3]),
        "method": "construct_vhost_nvme_controller"
    })
    for namespace in params[3][3]:
        vhost_nvme_json.append({
            "params": {
                "ctrlr": params[0][3],
                "bdev_name": namespace,
            },
            "method": "add_vhost_nvme_ns"
        })

    return vhost_nvme_json


def get_subsystem_json(config, section):
    params = [
        ["NQN", "nqn", str, ""],
        ["Host", "hosts", list, []],
        ["AllowAnyHost", "allow_any_host", bool, True],
        ["SN", "serial_number", str, ""],
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
            items = re.findall("\S+", value)
            adrfam = "IPv4"
            if len(items[1].split(":")) > 2:
                adrfam = "IPv6"
            listen_address.append({
                "trtype": items[0],
                "adrfam": adrfam,
                "trsvcid": items[1].split(":")[-1],
                "traddr": items[1].rsplit(":", 1)[0]
            })
        if "Namespace" == option:
            for item in value.split("\n"):
                items = re.findall("\S+", item)
                if len(items) == 2:
                    nsid = items[1]
                else:
                    nsid += 1
                namespaces.append({
                    "nsid": int(nsid),
                    "bdev_name": items[0],
                })
    parameters = to_json_params(params[0:4])
    parameters['listen_addresses'] = listen_address
    parameters['namespaces'] = namespaces
    nvmf_subsystem = {
        "params": parameters,
        "method": "construct_nvmf_subsystem"
    }

    if params[4][3]:
        nvmf_subsystem['params']['max_namespaces'] = int(params[4][3])

    return nvmf_subsystem


def get_nvmf_json(config):
    params = [
        ["AcceptorPollRate", "acceptor_poll_rate", int, 10000],
        ["MaxQueuesPerSession", "max_qpairs_per_ctrlr", int, 64],
        ["MaxQueueDepth", "max_queue_depth", int, 128],
        ["IncapsuleDataSize", "in_capsule_data_size", int, 4096],
        ["MaxIOSize", "max_io_size", int, 131072],
        ["IOUnitSize", "io_unit_size", int, 131072],
        ["MaxSubsystems", "max_subsystems", int, 1024]
    ]
    for option in config.options("Nvmf"):
        set_param(params, option, config.get("Nvmf", option))
    nvmf_json = []
    nvmf_json.append({
        "params": to_json_params([params[0]]),
        "method": "set_nvmf_target_config"
    })
    nvmf_json.append({
        "params": to_json_params(params[1:7]),
        "method": "set_nvmf_target_options"
    })

    return nvmf_json


def get_target_node_json(config, section):
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
            items = re.findall("\S+", value)
            pg_ig_maps.append({
                "ig_tag": initiator_groups_map[items[1]],
                "pg_tag": portal_groups_map[items[0]]
            })
        if "AuthMethod" == option:  # Auto
            pass
        if "AuthGroup" == option:  # AuthGroup1
            pass
        if "UseDigest" == option:
            if "Auto" == value:
                use_digest = True
        if re.match("LUN\d+", option):
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
        "method": "construct_target_node"
    }

    return target_json


def get_portal_group_json(config, name):
    global portal_groups_map
    global pg_tag_id
    portal_group_json = []
    pg_tag_id += 1
    portal_groups_map[name] = pg_tag_id
    portals = []
    cpumask = "0x1"
    for option in config.options(name):
        if "Portal" == option:
            for value in config.get(name, option).split("\n"):
                items = re.findall("\S+", value)
                portal = {'host': items[1].split(":")[0]}
                if "@" in items[1]:
                    portal['port'] = items[1].split(":")[1].split("@")[0]
                    portal['cpumask'] = items[1].split(":")[1].split("@")[1]
                else:
                    portal['port'] = items[1].split(":")[1]
                    portal['cpumask'] = cpumask
                portals.append(portal)

    portal_group_json.append({
        "params": {
            "portals": portals,
            "tag": portal_groups_map[name]
        },
        "method": "add_portal_group"
    })

    return portal_group_json


def get_initiator_group_json(config, name):
    global initiator_groups_map
    global ig_tag_id
    initiators = []
    netmasks = []
    ig_tag_id += 1
    initiator_groups_map[name] = ig_tag_id

    for option in config.options(name):
         if "InitiatorName" == option:
             initiators.append(config.get(name, option))
         if "Netmask" == option:
             netmasks.append(config.get(name, option))
    initiator_group_json = {
        "params": {
            "initiators": initiators,
            "tag": initiator_groups_map[name],
            "netmasks": netmasks
        },
        "method": "add_initiator_group"
    }

    return initiator_group_json


def get_bdev_json(config):
    params = [
        ["BdevIoPoolSize", "bdev_io_pool_size", int, 65536],
        ["BdevIoCacheSize", "bdev_io_cache_size", int, 256]
    ]
    for option in config.options("Bdev"):
        set_param(params, option, config.get("Bdev", option))

    return {"params": to_json_params(params), "method": "set_bdev_options"}


def get_pmem_json(config, section):
    pmem_json = []
    for option in config.options(section):
        if "Blk" == option:
            for value in config.get(section, option).split("\n"):
                items = re.findall("\S+", value)
                pmem_json.append({
                    "params": {
                        "name": items[1],
                        "pmem_file": items[0]
                    },
                    "method": "construct_pmem_bdev"
                })

    return pmem_json


def get_aio_json(config):
    aio_json = []
    value = None
    for option in config.options("AIO"):
        if option == "AIO":
            value = config.get("AIO", option).split("\n")
    for item in value:
        items = re.findall("\S+", item)
        params = {}
        params['filename'] = items[0]
        params['name'] = items[1]
        if len(items) == 3:
            params['block_size'] = int(items[2])
        aio_json.append({
            "params": params,
            "method": "construct_aio_bdev"
        })

    return aio_json


def get_virtio_user_json(config, section):
    params = [
        ["Path", "traddr", str, ""],
        ["Queues", "vq_count", int, 1],
        ["Type", "dev_type", "dev_type", "scsi"],
        ["Name", "name", str, section],
        ["TrType", "trtype", str, "user"],
        ["VqSize", "vq_size", int, 512]
    ]
    for option in config.options(section):
        set_param(params, option, config.get(section, option))
    dev_name = "Scsi"
    if params[2][3] == "blk":
        dev_name = "Blk"
    params[3][3] = params[3][3].replace("User", dev_name)

    return {
        "params": to_json_params(params),
        "method": "construct_virtio_dev"
    }


def get_nvme_json(config):
    params = [
        ["RetryCount", "retry_count", int, 4],
        ["TimeoutuSec", "timeout_us", int, 0],
        ["AdminPollRate", "nvme_adminq_poll_period_us", int, 1000000],
        ["ActionOnTimeout", "action_on_timeout", str, "none"],
        ["HotplugEnable", "enable", bool, False],
        ["AdminPollRate", "period_us", int, 1000]
    ]
    nvme_json = []
    for option in config.options("Nvme"):
        value = config.get("Nvme", option)
        if "TransportID" == option:
            entry = re.findall("\S+", value)
            nvme_name = entry[-1]
            trtype = re.findall("trtype:\S+", value)
            if trtype:
                trtype = trtype[0].replace("trtype:", "").replace("\"", "")
            traddr = re.findall("traddr:\S+", value)
            if traddr:
                traddr = traddr[0].replace("traddr:", "").replace("\"", "")
            nvme_json.append({
                "params": {
                    "trtype": trtype,
                    "name": nvme_name,
                    "traddr": traddr
                },
                "method": "construct_nvme_bdev"
            })
        else:
            set_param(params, option, value)
    params[3][3] = params[3][3].lower()
    params[5][3] = params[5][3] * 100
    nvme_json.append({
        "params": to_json_params(params[4:6]),
        "method": "set_bdev_nvme_hotplug"
    })
    nvme_json.append({
        "params": to_json_params(params[0:4]),
        "method": "set_bdev_nvme_options"
    })
    return nvme_json


def get_split_json(config):
    split_json = []
    value = []
    for option in config.options("Split"):
        if "Split" == option:
            value = config.get("Split", option)
    if value and not isinstance(value, list):
        value = [value]
    for split in value:
        items = re.findall("\S+", split)
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
            "method": "construct_split_vbdev"
        })

    return split_json


def get_malloc_json(config):
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
            "method": "construct_malloc_bdev"
        })

    return malloc_json


def get_iscsi_json(config, section):
    params = [
        ['AllowDuplicateIsid', 'allow_duplicated_isid', bool, False],
        ['DefaultTime2Retain', 'default_time2retain', int, 20],
        ['DiscoveryAuthMethod', 'req_discovery_auth_mutual', bool, False],
        ['MaxConnectionsPerSession', 'max_connections_per_session', int, 2],
        ['Timeout', 'nop_timeout', int, 60],
        ['DiscoveryAuthMethod', 'no_discovery_auth', bool, False],
        ['DiscoveryAuthMethod', 'req_discovery_auth', bool, False],
        ['NodeBase', 'node_base', str, "iqn.2016-06.io.spdk"],
        ['AuthFile', 'auth_file', str, "/usr/local/etc/spdk/auth.conf"],
        ['DiscoveryAuthGroup', 'discovery_auth_group', int, 0],
        ['MaxSessions', 'max_sessions', int, 128],
        ['ImmediateData', 'immediate_data', bool, True],
        ['ErrorRecoveryLevel', 'error_recovery_level', int, 0],
        ['NopInInterval', 'nop_in_interval', int, 30],
        ['MinConnectionsPerCore', 'min_connections_per_core', int, 4],
        ['DefaultTime2Wait', 'default_time2wait', int, 2],
        ['QueueDepth', 'max_queue_depth', int, 64]
    ]
    for option in config.options(section):
        set_param(params, option, config.get(section, option))
    return {"method": "set_iscsi_options", "params": to_json_params(params)}

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument('-old', dest='old')
    parser.add_argument('-new', dest='new', default=None)

    args = parser.parse_args()
    parse_old_config(args.old, args.new)
