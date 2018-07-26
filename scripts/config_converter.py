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


#dictionary with new config that will be written to new json config file
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
                #print("m.gr : %s" % m.group(0))
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

    #print("setup: %s" % setup)
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
        if index_found:
           if item['method'] in ["add_vhost_scsi_lun", "add_vhost_nvme_ns"]:
               i += 1
               continue
           else:
               break
        if item['method'] not in vhost_method_order:
            i += 1
            continue
        item_index = vhost_method_order[item['method']]
        if insert_index < item_index:
            index_found = True
            continue
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
            virtio_json = get_virtio_user_json(header_setup[header], header.replace("[", "").replace("]", ""))
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
            portal_group_json = get_portal_group_json(header_setup[header], header.replace("[", "").replace("]", ""))
            subsystem = get_subsystem("iscsi")
            for item in portal_group_json:
                index = get_iscsi_insert_index(item['method'])
                subsystem['config'].insert(index, item)
        if re.match("\[InitiatorGroup\d+\]", header):
            initiator_group_json = get_initiator_group_json(header_setup[header], header.replace("[", "").replace("]", ""))
            subsystem = get_subsystem("iscsi")
            index = get_iscsi_insert_index(initiator_group_json['method'])
            subsystem['config'].insert(index, initiator_group_json)
        if re.match("\[TargetNode\d+\]", header):
            target_node_json = get_target_node_json(header_setup[header], header.replace("[", "").replace("]", ""))
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
            index = -1
            for item in vhost_scsi_json:
                if index == -1:
                    index = get_vhost_insert_index(item['method'])
                else:
                    index += 1
                subsystem['config'].insert(index, item)
        if re.match("\[VhostBlk\d+\]", header):
            vhost_blk_json = get_vhost_blk_json(header_setup[header])
            subsystem = get_subsystem("vhost")
            index = -1
            for item in vhost_blk_json:
                if index == -1:
                    index = get_vhost_insert_index(item['method'])
                else:
                    index += 1
                subsystem['config'].insert(index, item)
        if re.match("\[VhostNvme\d+\]", header):
            vhost_nvme_json = get_vhost_nvme_json(header_setup[header])
            subsystem = get_subsystem("vhost")
            index = -1
            for item in vhost_nvme_json:
                if index == -1:
                    index = get_vhost_insert_index(item['method'])
                else:
                    index += 1
                subsystem['config'].insert(index, item)

    with open(new_config, "w") as new_file:
        #new_file.write(json.dumps(new_json_config, indent=2))
        json.dump(new_json_config, new_file, indent=2)
        new_file.write("\n")

def get_vhost_scsi_json(setup):
    name = None
    targets = []
    cpumask = None
    vhost_scsi_json = []
    for item in setup:
        item = re.findall("\S+", item)
        if "Name" == item[0]:
            name = item[1]
        if "Target" == item[0]:
            targets.append({
                "scsi_target_num": int(item[1]),
                "bdev_name": item[2],
                "ctrlr": name
            })
        if "Cpumask" in item[0]:
            cpumask = str(int(item[1], 16))

    vhost_scsi_json.append({
        "params": {
            "cpumask": cpumask,
            "ctrlr": name
        },
        "method": "construct_vhost_scsi_controller"
    })
    for target in targets:
        vhost_scsi_json.append({
            "params": {
                "scsi_target_num": target['scsi_target_num'],
                "ctrlr": target['ctrlr'],
                "bdev_name": target['bdev_name'],
                #"ctrlr": target['ctrlr'],
            },
            "method": "add_vhost_scsi_lun"
        })

    return vhost_scsi_json

def get_vhost_blk_json(setup):
    vhost_blk_json = []
    name = None
    dev = ""
    readonly = False
    cpumask = ""
    for item in setup:
        item = re.findall("\S+", item)
        if "Name" == item[0]:
            name = item[1]
        if "Dev" == item[0]:
            dev = item[1]
        if "ReadOnly" == item[0]:
            readonly = item[1]
            if "no" == readonly.lower():
                readonly = False
            else:
                readonly = True
        if "Cpumask" == item[0]:
            cpumask = str(int(item[1], 16))
    vhost_blk_json.append({
        "params": {
            "dev_name": dev,
            "readonly": readonly,
            "ctrlr": name,
            "cpumask": cpumask
        },
        "method": "construct_vhost_blk_controller"
    })

    return vhost_blk_json

def get_vhost_nvme_json(setup):
    vhost_nvme_json = []
    name = None
    number_of_queues = -1
    namespaces = []
    cpumask = None
    for item in setup:
        item = re.findall("\S+", item)
        if "Name" == item[0]:
            name = item[1]
        if "NumberOfQueues" == item[0]:
            number_of_queues = int(item[1])
        if "Namespace" == item[0]:
            namespaces.append({
                "bdev_name": item[1]
            })
        if "Cpumask" == item[0]:
            cpumask = str(int(item[1], 16))
    vhost_nvme_json.append({
        "params": {
            "cpumask": cpumask,
            "ctrlr": name,
            "io_queues": number_of_queues,
        },
        "method": "construct_vhost_nvme_controller"
    })
    for namespace in namespaces:
        vhost_nvme_json.append({
            "params": {
                "ctrlr": name,
                "bdev_name": namespace['bdev_name'],
            },
            "method": "add_vhost_nvme_ns"
        })


    return vhost_nvme_json

def get_subsystem_json(setup):
    nqn = ""
    hosts = []
    listen_address = []
    allow_any_host = True
    serial_number = ""
    max_namespaces = ""
    namespaces = []
    nsid = 0
    for item in setup:
        item = re.findall("\S+", item)
        if "NQN" == item[0]:
            nqn = item[1]
        if "Host" == item[0]:
            hosts.append(item[1])
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
        if "AllowAnyHost" == item[0]:
            if "No" == item[1]:
                allow_any_host = False
        if "SN" == item[0]:
            serial_number = item[1]
        if "MaxNamespaces" == item[0]:
            max_namespaces = item[1]
        if "Namespace" == item[0]:
            if len(item) == 3:
                nsid = item[2]
            else:
                nsid += 1
            namespaces.append({
                "nsid": int(nsid),
                "bdev_name": item[1],
            })

    nvmf_subsystem = {
          "params": {
            "listen_addresses": listen_address,
            "hosts": hosts,
            "namespaces": namespaces,
            "allow_any_host": allow_any_host,
            "serial_number": serial_number,
            "nqn": nqn
          },
          "method": "construct_nvmf_subsystem"
        }

    if max_namespaces:
        nvmf_subsystem['params']['max_namespaces'] = int(max_namespaces)

    return nvmf_subsystem

def get_nvmf_json(setup):
    nvmf_json = []
    in_capsule_data_size = 4096
    io_unit_size = 131072
    max_qpairs_per_ctrlr = 64
    max_queue_depth = 128
    max_io_size = 131072
    acceptor_poll_rate = 10000
    for item in setup:
        item = re.findall("\S+", item)
        if "AcceptorPollRate" == item[0]:
            acceptor_poll_rate = int(item[1])
        if "MaxQueuesPerSession" == item[0]:
            max_qpairs_per_ctrlr = int(item[1])
        if "MaxQueueDepth" == item[0]:
            max_queue_depth = int(item[1])
        if "InCapsuleDataSize" == item[0]:
            in_capsule_data_size = int(item[1])
        if "MaxIOSize" == item[0]:
            max_io_size = int(item[1])
        if "IOUnitSize" == item[0]:
            io_unit_size = int(item[1])
    nvmf_json.append({
        "params": {
            "acceptor_poll_rate": acceptor_poll_rate
        },
        "method": "set_nvmf_target_config"
    })
    nvmf_json.append({
        "params": {
            "in_capsule_data_size": in_capsule_data_size,
            "io_unit_size": io_unit_size,
            "max_qpairs_per_ctrlr": max_qpairs_per_ctrlr,
            "max_queue_depth": max_queue_depth,
            "max_io_size": max_io_size,
            "max_subsystems": 1024
        },
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
            name = item[1].split(":")[-1]
        if "TargetAlias" == item[0]:
            alias_name = " ".join(item[1:]).replace("\"", "")
        if "Mapping" == item[0]:
            pg_tag = portal_groups_map[item[1]]
            ig_tag = initiator_groups_map[item[2]]
            pg_ig_maps.append({
                "ig_tag": ig_tag,
                "pg_tag": pg_tag
            })
        if "AuthMethod" == item[0]:#Auto
            pass
        if "AuthGroup" == item[0]:#AuthGroup1
            pass
        if "UseDigest" == item[0]:
            if "Auto" == item[1]:
                use_digest = True
        if re.match("LUN\d+", item[0]):
            lun_id = len(luns)
            luns.append({"lun_id": lun_id,
                         "bdev_name": item[1]})
        if "QueueDepth" == item[0]:
            queue_depth = int(item[1])
            if queue_depth == 128:
                queue_depth = 64

    target_json = {
        "params": {
            "mutual_chap": mutual_chap,
            "name": "iqn.2016-06.io.spdk:%s" % name,
            "alias_name": alias_name,
            "luns": luns,
            "require_chap": require_chap,
            "chap_group": chap_group,
            "pg_ig_maps": pg_ig_maps,
            "data_digest": data_digest,
            "disable_chap": disable_chap,
            "header_digest": header_digest,
            "queue_depth": queue_depth
        },
        "method": "construct_target_node"
    }

    target_tmp_json = {'params': {}, 'method': target_json['method']}
    for k in sorted(target_json['params'], key=target_json['params'].get, reverse=True):
        target_tmp_json['params'][k] = target_json['params'][k]

    return target_tmp_json

def get_portal_group_json(setup, name):
    global portal_groups_map
    global pg_tag_id
    portal_group_json = []
    pg_tag_id += 1
    portal_groups_map[name] = pg_tag_id
    portals = []
    cpumask = "0x1"
    for item in setup:
        item = re.findall("\S+", item)
        if "Portal" == item[0]:
            portal = {}
            portal['host'] = item[2].split(":")[0]
            if "@" in item[2]:
                portal['port'] = item[2].split(":")[1].split("@")[0]
                portal['cpumask'] = item[2].split(":")[1].split("@")[1]
            else:
                portal['port'] = item[2].split(":")[1]
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
    bdev_json = {
        "params": {
            "bdev_io_pool_size": 65536,
            "bdev_io_cache_size": 256
        },
        "method": "set_bdev_options"
    }
    for item in setup:
        item = re.findall("\S+", item)
        if "BdevIoPoolSize" == item[0]:
            bdev_json['params']['bdev_io_pool_size'] = int(item[1])
        if "BdevIoCacheSize" == item[0]:
            bdev_json['params']['bdev_io_cache_size'] = int(item[1])

    return bdev_json

def get_pmem_json(setup):
    pmem_json = []
    for item in setup:
        item = re.findall("\S+", item)
        if "Blk" == item[0]:
            pmem_path = item[1]
            pmem_name = item[2]
            pmem_json.append({
                "params": {
                    "name": pmem_name,
                    "pmem_file": pmem_path
                },
                "method": "construct_pmem_bdev"
            })

    return pmem_json

def get_aio_json(setup):
    aio_json = []
    for item in setup:
        item = re.findall("\S+", item)
        if "AIO" == item[0]:
            params = {}
            params['filename'] = item[1]
            params['name'] = item[2]
            if len(item) == 4:
                params['block_size'] = int(item[3])
            aio_json.append({
                "params": params,
                "method": "construct_aio_bdev"
            })

    return aio_json

def get_virtio_user_json(setup, name):
    dev_type = "scsi"
    queues = 1
    for item in setup:
        item = re.findall("\S+", item)
        if "Path" == item[0]:
            path = item[1]
        if "Queues" == item[0]:
            queues = int(item[1])
        if "Type" == item[0]:
            type = item[1].lower()
            if type == "blk":
                dev_type = "blk"
        if "Name" == item[0]:
            name = item[1]
    if dev_type == "blk":
        name = name.replace("User", "Blk")
    else:
        name = name.replace("User", "Scsi")

    return {
        "params": {
            "name": name,
            "dev_type": dev_type,
            "vq_size": 512,
            "trtype": "user",
            "traddr": path,
            "vq_count": queues
        },
        "method": "construct_virtio_dev"
    }


def get_nvme_json(setup):
    nvme_json = []
    retry_count = 4
    timeout_us = 0
    nvme_adminq_poll_period_us = 1000000
    action_on_timeout = "none"
    hotplug_enable = False
    admin_poll_rate = 100000
    for item in setup:
        line = item
        item = re.findall("\S+", item)
        if "TransportID" == item[0]:
            nvme_name = item[-1]
            trtype = re.findall("trtype:\S+", line)
            if trtype:
                trtype = trtype[0].replace("trtype:", "").replace("\"", "")
            traddr = re.findall("traddr:\S+", line)
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
        if "RetryCount" == item[0]:
            retry_count = int(item[1])
        if "TimeoutUsec" == item[0]:
            timeout_us = int(item[1])
        if "ActionOnTimeout" == item[0]:
            action_on_timeout = item[1].lower()
        if "AdminPollRate" == item[0]:
            admin_poll_rate = int(item[1])*100
            nvme_adminq_poll_period_us = int(item[1])
        if "HotplugEnable" == item[0]:
            if "yes" == item[1].lower():
                hotplug_enable = True

    nvme_json.append({
        "params": {
            "enable": hotplug_enable,
            "period_us": admin_poll_rate
        },
        "method": "set_bdev_nvme_hotplug"
    })
    nvme_json.append({
        "params": {
            "retry_count": retry_count,
            "timeout_us": timeout_us,
            "nvme_adminq_poll_period_us": nvme_adminq_poll_period_us,
            "action_on_timeout": action_on_timeout
        },
        "method": "set_bdev_nvme_options"
    })
    return nvme_json

def get_split_json(item):
    item = item.split(" ")
    if "Split" == item[0]:
        base_bdev = item[1]
        split_count = int(item[2])
        if len(item) == 4:
            split_size_mb = item[3]
        else:
            split_size_mb = 0
        return {
            "params": {
                "base_bdev": base_bdev,
                "split_size_mb": split_size_mb,
                "split_count":split_count
            },
            "method": "construct_split_vbdev"
        }

def get_malloc_json(setup):
    malloc_json = []
    number_of_luns = 0
    lun_size_in_mb = 0
    block_size = 0
    for item in setup:
        item = item.replace("\"", "")
        item = re.findall("\S+", item)
        if "NumberOfLuns" == item[0]:
            number_of_luns = int(item[1])
        elif "LunSizeInMB" == item[0]:
            lun_size_in_mb = int(item[1])
        elif "BlockSize" == item[0]:
            block_size = int(item[1])

    for lun in range(0, number_of_luns):
        malloc_json.append({
            "params": {
                "block_size": block_size,
                "num_blocks": lun_size_in_mb * 1024 * 1024 / block_size,
                "name": "Malloc%s" % lun
            },
            "method": "construct_malloc_bdev"
        })

    return malloc_json

def get_iscsi_json(setup):
    iscsi_json = {
        "params": {
        },
        "method": "set_iscsi_options"
    }
    allow_duplicated_isid = False
    default_time2retain = 20
    req_discovery_auth_mutual = False
    max_connections_per_session = 2
    nop_timeout = 30
    no_discovery_auth = False
    req_discovery_auth = False
    node_base = "iqn.2016-06.io.spdk"
    auth_file = "/usr/local/etc/spdk/auth.conf"
    discovery_auth_group = 0
    max_sessions = 128
    immediate_data = True
    error_recovery_level = 0
    nop_in_interval = 30
    min_connections_per_core = 4
    default_time2wait = 2
    max_queue_depth = 64
    for item in setup:
        item = item.replace("\"", "")
        item = re.findall("\S+", item)
        if "NodeBase" == item[0]:
            node_base = item[1]
        elif "AuthFile" == item[0]:
            auth_file = item[1]
        elif "Timeout" == item[0]:
            nop_timeout = int(item[1])
        elif "DiscoveryAuthMethod" == item[0]:
             if "None" == item[1]:
                 no_discovery_auth = True
             elif item[1] == "CHAP":
                 req_discovery_auth = True
             elif item[1] == "Mutual":
                 req_discovery_auth = True
                 req_discovery_auth_mutual  = True
        elif "DiscoveryAuthGroup" == item[0]:
            discovery_auth_group = int(item[1].replace("AuthGroup", ""))
        elif "MaxSessions" == item[0]:
            max_sessions = int(item[1])
        elif "ImmediateData" == item[0]:
            immediate_data = False
            if 'yes' == item[1].lower():
                immediate_data = True
        elif "ErrorRecoveryLevel" == item[0]:
            error_recovery_level = int(item[1])
        elif "NopInInterval" == item[0]:
            nop_in_interval = int(item[1])
        elif "AllowDuplicateIsid" == item[0]:
            if "yes" == item[1].lower():
                allow_duplicated_isid = True
        elif "DefaultTime2Retain" == item[0]:
            default_time2retain = item[1]
        elif "DefaultTime2Wait" == item[0]:
            default_time2wait = int(item[1])
        elif "MaxConnectionsPerSession" == item[0]:
            max_connections_per_session = item[1]
        elif "QueueDepth" == item[0]:
            max_queue_depth = int(item[1])
        elif "MinConnectionsPerCore" == item[0]:
            min_connections_per_core = int(item[1])
    iscsi_json['params']['node_base'] = node_base
    iscsi_json['params']['auth_file'] = auth_file
    iscsi_json['params']['discovery_auth_group'] = discovery_auth_group
    iscsi_json['params']['max_sessions'] = max_sessions
    iscsi_json['params']['immediate_data'] = immediate_data
    iscsi_json['params']['error_recovery_level'] = error_recovery_level
    iscsi_json['params']['nop_in_interval'] = nop_in_interval
    iscsi_json['params']['min_connections_per_core'] = min_connections_per_core
    iscsi_json['params']['default_time2wait'] = default_time2wait
    iscsi_json['params']['max_queue_depth'] = max_queue_depth
    iscsi_json['params']['req_discovery_auth'] = req_discovery_auth
    iscsi_json['params']['no_discovery_auth'] = no_discovery_auth
    iscsi_json['params']['nop_timeout'] = nop_timeout
    iscsi_json['params']['max_connections_per_session'] = max_connections_per_session
    iscsi_json['params']['req_discovery_auth_mutual'] = req_discovery_auth_mutual
    iscsi_json['params']['default_time2retain'] = default_time2retain
    iscsi_json['params']['allow_duplicated_isid'] = allow_duplicated_isid

    return iscsi_json

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument('-old', dest='old')
    parser.add_argument('-new', dest='new', default=None)

    args = parser.parse_args()
    parse_old_config(args.old, args.new)
