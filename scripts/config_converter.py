import argparse
import re
import json

portal_groups_map = {}
initiator_groups_map = {}
global pi_tag_id
pi_tag_id = 0


new_json_config = {
    "subsystems": [
        {
            "subsystem": "copy",
            "config": []
        },
        {
            "subsystem": "interface",
            "config": []
        },
        {
            "subsystem": "net_framework",
            "config": []
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


def get_headers(config):
    headers = []
    with open(config, 'r') as config_file:
        for line in config_file:
            m = re.search('\[.*\]', line)
            if m:
                print("m.gr : %s" % m.group(0))
                headers.append(m.group(0))

    return headers

def get_header_setup(header, config):
    setup = []
    with open(config, 'r') as config_file:
        for line in config_file:
            if header in line:
                in_setup = True
                continue
            m = re.search('\[.*\]', line)
            if m:
                in_setup = False
                continue
            if in_setup:
                if line.strip():
                    setup.append(line.strip())

    print "setup: %s" % setup

    return setup

def parse_old_config(old_config, new_config):
    headers = get_headers(old_config)
    #print("headers: %s" % headers)
    header_setup = {}
    for header in headers:
        header_setup[header] = get_header_setup(header, old_config)

    parsed_headers={}
    for header in headers:
        parsed_headers[header] = False

    for header in headers:
        if parsed_headers[header] is True:
            continue
        if header == "[Nvme]":
            nvme_json = get_nvme_json(header_setup[header])
            for subsystem in new_json_config['subsystems']:
                if subsystem['subsystem'] == "bdev":
                    for item in nvme_json:
                        subsystem['config'].append(item)
                    print "subsystem: %s" % subsystem
        if header == "[Malloc]":
            malloc_json = get_malloc_json(header_setup[header])
            for subsystem in new_json_config['subsystems']:
                if subsystem['subsystem'] == "bdev":
                    for item in malloc_json:
                        subsystem['config'].append(item)
                    print "subsystem: %s" % subsystem
        if re.match("\[VirtioUser\d+\]", header):
            virtio_json = get_virtio_user_json(header_setup[header], header.replace("[", "").replace("]", ""))
            for subsystem in new_json_config['subsystems']:
                if subsystem['subsystem'] == "bdev":
                    subsystem['config'].append(virtio_json)
                    print "subsystem: %s" % subsystem
        if header == "[Split]":
            for setup in header_setup[header]:
                split_json = get_split_json(setup)
                for subsystem in new_json_config['subsystems']:
                    if subsystem['subsystem'] == "bdev":
                        subsystem['config'].append(split_json)
                        print "subsystem: %s" % subsystem
        if header == "[iSCSI]":
            iscsi_json = get_iscsi_json(header_setup[header])
            for subsystem in new_json_config['subsystems']:
                if subsystem['subsystem'] == "iscsi":
                    subsystem['config'].append(iscsi_json)
                    print "subsystem: %s" % subsystem
        if header == "[Pmem]":
            pmem_json = get_pmem_json(header_setup[header])
            for subsystem in new_json_config['subsystems']:
                if subsystem['subsystem'] == "bdev":
                    for item in pmem_json:
                        subsystem['config'].append(item)
                    print "subsystem: %s" % subsystem
        if header == "[AIO]":
            aio_json = get_aio_json(header_setup[header])
            for subsystem in new_json_config['subsystems']:
                if subsystem['subsystem'] == "bdev":
                    for item in aio_json:
                        subsystem['config'].append(item)
                    print "subsystem: %s" % subsystem
        if header == "[Bdev]":
            bdev_json = get_bdev_json(header_setup[header])
            for subsystem in new_json_config['subsystems']:
                if subsystem['subsystem'] == "bdev":
                    subsystem['config'].append(bdev_json)
                    print "subsystem: %s" % subsystem
        if re.match("\[PortalGroup\d+\]", header):
            portal_group_json = get_portal_group_json(header_setup[header], header.replace("[", "").replace("]", ""))
            for subsystem in new_json_config['subsystems']:
                if subsystem['subsystem'] == "iscsi":
                    for item in portal_group_json:
                        subsystem['config'].append(item)
                    print "subsystem: %s" % subsystem
        if re.match("\[InitiatorGroup\d+\]", header):
            initiator_group_json = get_initiator_group_json(header_setup[header], header.replace("[", "").replace("]", ""))
            for subsystem in new_json_config['subsystems']:
                if subsystem['subsystem'] == "iscsi":
                    subsystem['config'].append(initiator_group_json)
                    print "subsystem: %s" % subsystem
        if re.match("\[TargetNode\d+\]", header):
            target_node_json = get_target_node_json(header_setup[header], header.replace("[", "").replace("]", ""))
            for subsystem in new_json_config['subsystems']:
                if subsystem['subsystem'] == "iscsi":
                    subsystem['config'].append(target_node_json)
                    print "subsystem: %s" % subsystem
        if re.match("\[Nvmf\]", header):
            nvmf_json = get_nvmf_json(header_setup[header])
            for subsystem in new_json_config['subsystems']:
                if subsystem['subsystem'] == "nvmf":
                    for item in nvmf_json:
                        subsystem['config'].append(item)
                    print "subsystem: %s" % subsystem
        if re.match("\[Subsystem\d+\]", header):
            subsystem_json = get_subsystem_json(header_setup[header])
            for subsystem in new_json_config['subsystems']:
                if subsystem['subsystem'] == "nvmf":
                    subsystem['config'].append(subsystem_json)
                    print "subsystem: %s" % subsystem
        if re.match("\[VhostScsi\d+\]", header):
            vhost_scsi_json = get_vhost_scsi_json(header_setup[header])
            for subsystem in new_json_config['subsystems']:
                if subsystem['subsystem'] == "vhost":
                    for item in vhost_scsi_json:
                        subsystem['config'].append(item)
                    print "subsystem: %s" % subsystem
        if re.match("\[VhostBlk\d+\]", header):
            vhost_blk_json = get_vhost_blk_json(header_setup[header])
            for subsystem in new_json_config['subsystems']:
                if subsystem['subsystem'] == "vhost":
                    for item in vhost_blk_json:
                        subsystem['config'].append(item)
                    print "subsystem: %s" % subsystem
        if re.match("\[VhostNvme\d+\]", header):
            vhost_nvme_json = get_vhost_nvme_json(header_setup[header])
            for subsystem in new_json_config['subsystems']:
                if subsystem['subsystem'] == "vhost":
                    for item in vhost_nvme_json:
                        subsystem['config'].append(item)
                    print "subsystem: %s" % subsystem
        parsed_headers[header] = True
    with open(new_config, "w") as new_file:
        new_file.write(json.dumps(new_json_config, indent=2))

def get_vhost_scsi_json(setup):
    name = None
    targets = []
    cpumask = None
    vhost_scsi_json = []
    for item in setup:
        item = re.findall("\S+", item)
        if "Name" == item:
            name = item[1]
        if "Target" in item:
            targets.append({
                "scsi_target_num": item[1],
                "ctrlr": name,
                "bdev_name": item[2],
            })
        if "Cpumask" in item:
            cpumask = item[1]

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
                "bdev_name": target['bdev_name'],
                "ctrlr": target['ctrlr']
            },
            "method": "add_vhost_scsi_lun"
        })

    return vhost_scsi_json

def get_vhost_blk_json(setup):
    vhost_blk_json = []
    name = None
    dev = ""
    readonly = ""
    cpumask = ""
    for item in setup:
        item = re.findall("\S+", item)
        if "Name" == item[0]:
            name = item[1]
        if "Dev" == item:
            dev = item[1]
        if "Readonly" == item:
            readonly = item[1]
            if "No" in readonly:
                readonly = False
            else:
                readonly = True
        if "Cpumask" == item[0]:
            cpumask = item[1]
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
            number_of_queues = item[1]
        if "Namespace" == item[0]:
            namespaces.append({
                "bdev_name": item[1]
            })
        if "Cpumask" == item[0]:
            cpumask = item[1]
    vhost_nvme_json.append({
        "params": {
            "cpumask": cpumask,
            "io_queues": number_of_queues,
            "ctrlr": name
        },
        "method": "construct_vhost_nvme_controller"
    })
    for namespace in namespaces:
        vhost_nvme_json.append({
            "params": {
                "bdev_name": namespace['bdev_name'],
                "ctrlr": name
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
        if "NQN" == item:
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
        if "Namespace " == item[0]:
            if len(item) == 3:
                nsid = item[2]
            else:
                nsid += 1
            namespaces.append({
                "bdev_name": item[1],
                "nsid": nsid
            })

    return {
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


def get_nvmf_json(setup):
    nvmf_json = []
    in_capsule_data_size = 4096
    io_unit_size = 131072
    max_qpairs_per_ctrlr = 64
    max_queue_depth = 128
    max_io_size = 131072
    for item in setup:
        item = re.findall("\S+", item)
        if "AcceptorPollRate" == item[0]:
            nvmf_json.append({
                "params": {
                    "acceptor_poll_rate": item[1]
                },
                "method": "set_nvmf_target_config"
            })
        if "MaxQueuesPerSession" == item[0]:
            max_qpairs_per_ctrlr = item[1]
        if "MaxQueueDepth" in item[0]:
            max_queue_depth = item[1]
        if "InCapsuleDataSize" in item[0]:
            in_capsule_data_size = item[1]
        if "MaxIOSize" in item[0]:
            max_io_size = item[1]
        if "IOUnitSize" in item[0]:
            io_unit_size = item[1]
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
    chap_group = 0
    pg_ig_maps = []
    data_digest = False
    disable_chap = True
    header_digest = False
    queue_depth = 64

    for item in setup:
        item = re.findall("\S+", item)
        if "TargetName" in item[0]:
            name = item[1]
        if "TargetAlias" == item[0]:
            alias_name = item[1].replace("\"", "")
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
                data_digest = True
        if re.match("LUN\d+", item[0]):
            lun_id = len(luns)
            luns.append({"lun_id": lun_id,
                         "bdev_name": item[1]})
        if "QueueDepth" == item:
            queue_depth = item[1]

    return {
        "params": {
            "luns": luns,
            "mutual_chap": mutual_chap,
            "name": name,
            "alias_name": alias_name,
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

def get_portal_group_json(setup, name):
    global portal_groups_map
    global pi_tag_id
    portal_group_json = []
    pi_tag_id += 1
    portal_groups_map[name] = pi_tag_id
    portals = []
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
            portals.append(portal)
    portal_group_json.append({
        "params": {
            "portals": [portals],
            "tag": portal_groups_map[name]
        },
        "method": "add_portal_group"
    })

    return portal_group_json

def get_initiator_group_json(setup, name):
    global initiator_groups_map
    global pi_tag_id
    initiators = []
    netmasks = []
    pi_tag_id += 1
    initiator_groups_map[name] = pi_tag_id
    for item in setup:
        item = re.findall("\S+", item)
        if "InitiatorName" == item:
            initiators.append(item[1])
        if "Netmask" == item:
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
            if len(aio_split) == 4:
                params['block_size'] = item[3]
            aio_json.append({
                "params": params,
                "method": "construct_aio_bdev"
            })

    return aio_json

def get_virtio_user_json(setup, name):
    dev_type = "scsi"
    for item in setup:
        item = re.findall("\S+", item)
        if "Path" == item[0]:
            path = item[1]
        if "Queues" == item[0]:
            queues = item[1]
        if "Type" == item[0]:
            type = item[1].lower()
            if type == "blk":
                dev_type = "blk"
        if "Name" in item[0]:
            name = item[1]

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
    nvme_options = {
          "params": {
          },
          "method": "set_nvme_options"
        }
    for item in setup:
        print "item: %s" % item
        if "TransportID" in item:
            nvme = item.split(" ", 1)[1].rsplit(" ", 1)
            nvme_name = item.rsplit(" ", 1)[-1]
            print nvme
            print nvme[0].replace("\"", "").replace("trtype:", "").replace("traddr:", "")
            trtype, traddress = nvme[0].replace("\"", "").replace("trtype:", "").replace("traddr:", "").split(" ")
            nvme_json.append({
              "params": {
                "trtype": trtype,
                "name": nvme_name,
                "traddr": traddress
              },
              "method": "construct_nvme_bdev"
            })
        if "RetryCount" in item:
            nvme_options['params']['retry_count'] = item.split(" ")[-1]
        if "Timeout" in item:
            nvme_options['params']['timeout'] = item.split(" ")[-1]
        if "ActionOnTimeout" in item:
            nvme_options['params']['action_on_timeout'] = item.split(" ")[-1]
        if "AdminPollRate" in item:
            nvme_options['params']['admin_poll_rate'] = item.split(" ")[-1]

    nvme_json.append(nvme_options)
    return nvme_json

def get_split_json(item):
    if "Split" in item:
        split = item.split(" ")
        base_bdev = split[1]
        split_count = split[2]
        if len(split) == 4:
            split_size_mb = split[3]
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
        if "NumberOfLuns" in item:
            number_of_luns = int(item.split(" ")[-1])
        elif "LunSizeInMB" in item:
            lun_size_in_mb = int(item.split(" ")[-1])
        elif "BlockSize" in item:
            block_size = int(item.split(" ")[-1])

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
    for item in setup:
        item = item.replace("\"", "")
        if "NodeBase" in item:
            iscsi_json['params']['node_base'] = item.split(" ")[-1]
        elif "AuthFile" in item:
            iscsi_json['params']['auth_file'] = item.split(" ")[-1]
        elif "Timeout" in item:
            iscsi_json['params']['timeout'] = item.split(" ")[-1]
        elif "DiscoveryAuthMethod" in item:
            iscsi_json['params']['discobery_auth_method'] = item.split(" ")[-1]
        elif "DiscoveryAuthGroup" in item:
            iscsi_json['params']['discovery_auth_group'] = item.split(" ")[-1]
        elif "MaxSessions" in item:
            iscsi_json['params']['max_session'] = item.split(" ")[-1]
        elif "ImmediateData" in item:
            iscsi_json['params']['immediate_data'] = item.split(" ")[-1]
        elif "ErrorRecoveryLevel" in item:
            iscsi_json['params']['error_recovery_level'] = item.split(" ")[-1]
        elif "MaxR2T" in item:
            iscsi_json['params']['max_r2t'] = item.split(" ")[-1]
        elif "NopInInterval" in item:
            iscsi_json['params']['nop_in_interval'] = item.split(" ")[-1]
        elif "AllowDuplicatedIsid" in item:
            iscsi_json['params']['allow_duplicated_isid'] = item.split(" ")[-1]
        elif "DefaultTime2Retain" in item:
            iscsi_json['params']['default_time2retain'] = item.split(" ")[-1]
        elif "DefaultTime2Wait" in item:
            iscsi_json['params']['default_time2wait'] = item.split(" ")[-1]
        elif "MaxConnectionsPerSession" in item:
            iscsi_json['params']['max_connections_per_session'] = item.split(" ")[-1]
        elif "QueueDepth" in item:
            iscsi_json['params']['max_queue_depth'] = item.split(" ")[-1]
        elif "MinConnectionsPerCore" in item:
            iscsi_json['params']['min_connections_per_core'] = item.split(" ")[-1]

    return iscsi_json

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument('-old', dest='old')
    parser.add_argument('-new', dest='new')

    args = parser.parse_args()
    parse_old_config(args.old, args.new)
