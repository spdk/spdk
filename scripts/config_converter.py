#!/usr/bin/env python

import argparse
import re
import json

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
            "subsystem": "nbd",
            "config": []
        },
        {
            "subsystem": "nvmf",
            "config": []
        },
        {
            "subsystem": "scsi",
            "config": None
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


def remove_comments(line):
    return line.split("#", 1)[0].strip()


def get_headers(config):
    headers = []
    with open(config, 'r') as config_file:
        for line in config_file:
            # Remove comments
            line = remove_comments(line)
            if not line:
                continue
            m = re.search('\[.*\]', line)
            if m:
                headers.append(m.group(0))

    return headers


def get_header_setup(header, config):
    setup = []
    with open(config, 'r') as config_file:
        for line in config_file:
            line = remove_comments(line)
            if not line:
                continue
            if header in line:
                in_setup = True
                continue
            m = re.search('\[.*\]', line)
            if m:
                in_setup = False
                continue
            if in_setup:
                setup.append(line.strip())

    return setup


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
    # Get sections from old config
    headers = get_headers(old_config)
    header_setup = {}
    # Get settings of given section
    for header in headers:
        header_setup[header] = get_header_setup(header, old_config)

    # Add missing sections
    for section in ['[Nvme]', '[Nvmf]', '[Bdev]', '[iSCSI]']:
        if section not in headers:
            headers.append(section)
            header_setup[section] = []

    header_set = []
    for header in headers:
        if header not in header_set:
            header_set.append(header)

    for header in header_set:
        if header == "[Nvme]":
            nvme_json = get_nvme_json(header_setup[header])
            subsystem = get_subsystem("bdev")
            for item in nvme_json:
                index = get_bdev_insert_index(item['method'])
                subsystem['config'].insert(index, item)
        if header == "[Malloc]":
            malloc_json = get_malloc_json(header_setup[header])
            subsystem = get_subsystem("bdev")
            for item in malloc_json:
                index = get_bdev_insert_index(item['method'])
                subsystem['config'].insert(index, item)
        if re.match("\[VirtioUser\d+\]", header):
            virtio_json = get_virtio_user_json(header_setup[header],
                                               header.replace(
                                                   "[", "").replace("]", ""))
            subsystem = get_subsystem("bdev")
            index = get_bdev_insert_index(virtio_json['method'])
            subsystem['config'].insert(index, virtio_json)
        if header == "[Split]":
            for setup in header_setup[header]:
                split_json = get_split_json(setup)
                subsystem = get_subsystem("bdev")
                index = get_bdev_insert_index(split_json['method'])
                subsystem['config'].insert(index, split_json)
        if header == "[iSCSI]":
            iscsi_json = get_iscsi_json(header_setup[header])
            subsystem = get_subsystem("iscsi")
            index = get_iscsi_insert_index(iscsi_json['method'])
            subsystem['config'].insert(index, iscsi_json)
        if header == "[Pmem]":
            pmem_json = get_pmem_json(header_setup[header])
            subsystem = get_subsystem("bdev")
            for item in pmem_json:
                index = get_bdev_insert_index(item['method'])
                subsystem['config'].insert(index, item)
        if header == "[AIO]":
            aio_json = get_aio_json(header_setup[header])
            subsystem = get_subsystem("bdev")
            for item in aio_json:
                index = get_bdev_insert_index(item['method'])
                subsystem['config'].insert(index, item)
        if header == "[Bdev]":
            bdev_json = get_bdev_json(header_setup[header])
            subsystem = get_subsystem("bdev")
            index = get_bdev_insert_index(bdev_json['method'])
            subsystem['config'].insert(index, bdev_json)
        if re.match("\[PortalGroup\d+\]", header):
            portal_group_json = get_portal_group_json(
                                    header_setup[header],
                                    header.replace("[", "").replace("]", ""))
            subsystem = get_subsystem("iscsi")
            for item in portal_group_json:
                index = get_iscsi_insert_index(item['method'])
                subsystem['config'].insert(index, item)
        if re.match("\[InitiatorGroup\d+\]", header):
            initiator_group_json = get_initiator_group_json(
                                       header_setup[header],
                                       header.replace(
                                           "[", "").replace("]", ""))
            subsystem = get_subsystem("iscsi")
            index = get_iscsi_insert_index(initiator_group_json['method'])
            subsystem['config'].insert(index, initiator_group_json)
        if re.match("\[TargetNode\d+\]", header):
            target_node_json = get_target_node_json(
                                   header_setup[header],
                                   header.replace("[", "").replace("]", ""))
            subsystem = get_subsystem("iscsi")
            index = get_iscsi_insert_index(target_node_json['method'])
            subsystem['config'].insert(index, target_node_json)
        if re.match("\[Nvmf\]", header):
            nvmf_json = get_nvmf_json(header_setup[header])
            subsystem = get_subsystem("nvmf")
            for item in nvmf_json:
                subsystem['config'].append(item)
        if re.match("\[Subsystem\d+\]", header):
            subsystem_json = get_subsystem_json(header_setup[header])
            subsystem = get_subsystem("nvmf")
            subsystem['config'].append(subsystem_json)
        if re.match("\[VhostScsi\d+\]", header):
            vhost_scsi_json = get_vhost_scsi_json(header_setup[header])
            subsystem = get_subsystem("vhost")
            index = get_vhost_insert_index(vhost_scsi_json[0]['method'])
            for item in vhost_scsi_json:
                subsystem['config'].insert(index, item)
                index += 1
        if re.match("\[VhostBlk\d+\]", header):
            vhost_blk_json = get_vhost_blk_json(header_setup[header])
            subsystem = get_subsystem("vhost")
            index = get_vhost_insert_index(vhost_blk_json['method'])
            subsystem['config'].insert(index, vhost_blk_json)
        if re.match("\[VhostNvme\d+\]", header):
            vhost_nvme_json = get_vhost_nvme_json(header_setup[header])
            subsystem = get_subsystem("vhost")
            index = get_vhost_insert_index(vhost_nvme_json[0]['method'])
            for item in vhost_nvme_json:
                subsystem['config'].insert(index, item)
                index += 1
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
"""	raise Exception (message) """


def to_json_params(params):
    out = {}
    for param in params:
        out[param[1]] = param[3]
    return out


def get_vhost_scsi_json(setup):
    params = [
        ["Name", "ctrlr", str, None],
        ["Cpumask", "cpumask", "hex", "1"],
    ]
    targets = []
    vhost_scsi_json = []
    for item in setup:
        entry = re.search("(Name|Cpumask)[\s]+(\S+)", item)
        if entry:
            set_param(params, entry.groups()[0], entry.groups()[1])
            continue
        items = re.findall("\S+", item)
        if "Target" == items[0]:
            targets.append({
                "scsi_target_num": int(items[1]),
                "ctrlr": params[0][3],
                "bdev_name": items[2]
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


def get_vhost_blk_json(setup):
    params = [
        ["ReadOnly", "readonly", bool, False],
        ["Dev", "dev_name", str, ""],
        ["Name", "ctrlr", str, ""],
        ["Cpumask", "cpumask", "hex", ""]
    ]
    for item in setup:
        item = re.search("(Name|Dev|ReadOnly|Cpumask)[\s]+(\S+)", item)
        if not item:
            continue
        set_param(params, item.groups()[0], item.groups()[1])
    return {"method": "construct_vhost_blk_controller",
            "params": to_json_params(params)}


def get_vhost_nvme_json(setup):
    params = [
        ["Name", "ctrlr", str, ""],
        ["NumberOfQueues", "io_queues", int, -1],
        ["Cpumask", "cpumask", "hex", 0x1],
        ["Namespace", "bdev_name", list, []]
    ]
    search_pattern = "|".join(param[0] for param in params)
    for item in setup:
        item = re.search("({0})[\s]+(\S+)".format(search_pattern), item)
        if not item:
            continue
        set_param(params, item.groups()[0], item.groups()[1])
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


def get_subsystem_json(setup):
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
    search_pattern = "|".join(param[0] for param in params)
    for item in setup:
        entry = re.search("({0})[\s]+(\S+)".format(search_pattern), item)
        if entry:
            set_param(params, entry.groups()[0], entry.groups()[1])
            continue
        item = re.findall("\S+", item)
        if "Listen" == item[0]:
            adrfam = "IPv4"
            if len(item[2].split(":")) > 2:
                adrfam = "IPv6"
            listen_address.append({
                "trtype": item[1],
                "adrfam": adrfam,
                "trsvcid": item[2].split(":")[-1],
                "traddr": item[2].rsplit(":", 1)[0]
            })
        if "Namespace" == item[0]:
            if len(item) == 3:
                nsid = item[2]
            else:
                nsid += 1
            namespaces.append({
                "nsid": int(nsid),
                "bdev_name": item[1],
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


def get_nvmf_json(setup):
    params = [
        ["AcceptorPollRate", "acceptor_poll_rate", int, 10000],
        ["MaxQueuesPerSession", "max_qpairs_per_ctrlr", int, 64],
        ["MaxQueueDepth", "max_queue_depth", int, 128],
        ["InCapsuleDataSize", "in_capsule_data_size", int, 4096],
        ["MaxIOSize", "max_io_size", int, 131072],
        ["IOUnitSize", "io_unit_size", int, 131072],
        ["MaxSubsystems", "max_subsystems", int, 1024]
    ]
    pattern = "|".join(param[0] for param in params)
    for item in setup:
        item = re.search("({0})[\s]+(\S+)".format(pattern), item)
        if not item:
            continue
        set_param(params, item.groups()[0], item.groups()[1])
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


def get_target_node_json(setup, name):
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

    for item in setup:
        item = re.findall("\S+", item)
        if "TargetName" in item[0]:
            name = item[1]
        if "TargetAlias" == item[0]:
            alias_name = " ".join(item[1:]).replace("\"", "")
        if "Mapping" == item[0]:
            pg_ig_maps.append({
                "ig_tag": initiator_groups_map[item[2]],
                "pg_tag": portal_groups_map[item[1]]
            })
        if "AuthMethod" == item[0]:  # Auto
            pass
        if "AuthGroup" == item[0]:  # AuthGroup1
            pass
        if "UseDigest" == item[0]:
            if "Auto" == item[1]:
                use_digest = True
        if re.match("LUN\d+", item[0]):
            luns.append({"lun_id": len(luns),
                         "bdev_name": item[1]})
        if "QueueDepth" == item[0]:
            queue_depth = int(item[1])

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


def get_portal_group_json(setup, name):
    global portal_groups_map
    global pg_tag_id
    portal_group_json = []
    pg_tag_id += 1
    portal_groups_map[name] = pg_tag_id
    portals = []
    cpumask = "0x1"
    for item in setup:
        items = re.findall("\S+", item)
        if "Portal" == items[0]:
            portal = {'host': items[2].split(":")[0]}
            if "@" in items[2]:
                portal['port'] = items[2].split(":")[1].split("@")[0]
                portal['cpumask'] = items[2].split(":")[1].split("@")[1]
            else:
                portal['port'] = items[2].split(":")[1]
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


def get_initiator_group_json(setup, name):
    global initiator_groups_map
    global ig_tag_id
    initiators = []
    netmasks = []
    ig_tag_id += 1
    initiator_groups_map[name] = ig_tag_id
    for item in setup:
        item = re.findall("\S+", item)
        if "InitiatorName" == item[0]:
            initiators.append(item[1])
        if "Netmask" == item[0]:
            netmasks.append(item[1])
    initiator_group_json = {
        "params": {
            "initiators": initiators,
            "tag": initiator_groups_map[name],
            "netmasks": netmasks
        },
        "method": "add_initiator_group"
    }

    return initiator_group_json


def get_bdev_json(setup):
    params = [
        ["BdevIoPoolSize", "bdev_io_pool_size", int, 65536],
        ["BdevIoCacheSize", "bdev_io_cache_size", int, 256]
    ]
    for item in setup:
        item = re.search("(BdevIoPoolSize|BdevIoCacheSize)[\s]+(\S+)", item)
        if not item:
            continue
        set_param(params, item.groups()[0], item.groups()[1])

    return {"params": to_json_params(params), "method": "set_bdev_options"}


def get_pmem_json(setup):
    pmem_json = []
    for item in setup:
        items = re.findall("\S+", item)
        if "Blk" == items[0]:
            pmem_json.append({
                "params": {
                    "name": items[2],
                    "pmem_file": items[1]
                },
                "method": "construct_pmem_bdev"
            })

    return pmem_json


def get_aio_json(setup):
    aio_json = []
    for item in setup:
        items = re.findall("\S+", item)
        if "AIO" == items[0]:
            params = {}
            params['filename'] = items[1]
            params['name'] = items[2]
            if len(items) == 4:
                params['block_size'] = int(items[3])
            aio_json.append({
                "params": params,
                "method": "construct_aio_bdev"
            })

    return aio_json


def get_virtio_user_json(setup, name):
    params = [
        ["Path", "path", str, ""],
        ["Queues", "vq_count", int, 1],
        ["Type", "dev_type", "dev_type", "scsi"],
        ["Name", "name", str, name],
        ["TrType", "trtype", str, "user"],
        ["VqSize", "vq_size", int, 512]
    ]
    search_pattern = "|".join(param[0] for param in params[0:5])
    for item in setup:
        entry = re.search("({0})[\s]+(\S+)".format(search_pattern), item)
        if not entry:
            continue
        set_param(params, entry.groups()[0], entry.groups()[1])
    dev_name = "Scsi"
    if dev_type == "blk":
        dev_name = "Blk"
    params[3][3] = params[3][3].replace("User", dev_name)

    return {
        "params": to_json_params(params),
        "method": "construct_virtio_dev"
    }


def get_nvme_json(setup):
    params = [
        ["RetryCount", "retry_count", int, 4],
        ["TimeoutUsec", "timeout_us", int, 0],
        ["AdminPollRate", "nvme_adminq_poll_period_us", int, 1000000],
        ["ActionOnTimeout", "action_on_timeout", str, "none"],
        ["HotplugEnable", "enable", bool, False],
        ["AdminPollRate", "period_us", int, 100000]
    ]
    nvme_json = []
    search_pattern = "|".join(param[0] for param in params)
    for item in setup:
        entry = re.search("({0})[\s]+(\S+)".format(search_pattern), item)
        if entry:
            set_param(params, entry.groups()[0], entry.groups()[1])
            continue
        entry = re.findall("\S+", item)
        if "TransportID" == entry[0]:
            nvme_name = entry[-1]
            trtype = re.findall("trtype:\S+", item)
            if trtype:
                trtype = trtype[0].replace("trtype:", "").replace("\"", "")
            traddr = re.findall("traddr:\S+", item)
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


def get_split_json(item):
    items = re.findall("\S+", item)
    if "Split" == items[0]:
        split_size_mb = 0
        base_bdev = items[1]
        split_count = int(items[2])
        if len(items) == 4:
            split_size_mb = items[3]
        return {
            "params": {
                "base_bdev": base_bdev,
                "split_size_mb": split_size_mb,
                "split_count": split_count
            },
            "method": "construct_split_vbdev"
        }


def get_malloc_json(setup):
    malloc_json = []
    params = [
        ['NumberOfLuns', '', int, -1],
        ['LunSizeInMB', '', int, 20],
        ['BlockSize', '', int, 512]
    ]
    pattern = "|".join(param[0] for param in params)
    for item in setup:
        item = re.search("({0})[\s]+(\S+)".format(pattern), item)
        if not item:
            continue
        set_param(params, item.groups()[0], item.groups()[1])
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


def get_iscsi_json(setup):
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
    search_pattern = "|".join(param[0] for param in params)
    for item in setup:
        item = re.search("({0})[\s]+(\S+)".format(search_pattern), item)
        if not item:
            continue
        set_param(params, item.groups()[0], item.groups()[1])
    return {"method": "set_iscsi_options", "params": to_json_params(params)}

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument('-old', dest='old')
    parser.add_argument('-new', dest='new', default=None)

    args = parser.parse_args()
    parse_old_config(args.old, args.new)
