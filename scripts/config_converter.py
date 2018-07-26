import argparse
import re
import json


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
    print("headers: %s" % headers)
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
        parsed_headers[header] = True
    with open(new_config, "w") as new_file:
        new_file.write(json.dumps(new_json_config, indent=2))


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
        if "TargetName" in item:
            name = item.split(" ")[-1]
        if "TargetAlias" in item:
            alias_name = item.split(" ", 1)[-1].replace("\"", "")
        if "Mapping" in item:
            pass
        if "AuthMethod" in item:#Auto
            pass
        if "AuthGroup" in item:#AuthGroup1
            pass
        if "UseDigest":
            if "Auto" in item.split(" ")[-1]:
                data_digest = True
        if re.match("LUN\d+", item):
            lun_id = len(luns)
            luns.append({"lun_id": lun_id,
                         "bdev_name": item.split(" ")[-1]})
        if "QueueDepth" in item:
            queue_depth = item.split(" ")[-1]

    return {
        "params": {
            "luns": luns,
            "mutual_chap": mutual_chap,
            "name": name,
            "alias_name": alias_name,
            "require_chap": require_chap,
            "chap_group": chap_group,
            "pg_ig_maps": [
                {
                    "ig_tag": 2,
                    "pg_tag": 1
                }
            ],
            "data_digest": data_digest,
            "disable_chap": disable_chap,
            "header_digest": header_digest,
            "queue_depth": queue_depth
        },
        "method": "construct_target_node"
    }

def get_portal_group_json(setup, name):
    portal_group_json = []

    for item in setup:
        if "Portal" in item:
            split_portal = item.split(" ")
            tag = int(split_portal[1].replace("DA", ""))
            portals = {}
            portals['host'] = split_portal[2].split(":")[0]
            if "@" in split_portal[2]:
                portals['port'] = split_portal[2].split(":")[1].split("@")[0]
                portals['cpumask'] = split_portal[2].split(":")[1].split("@")[1]
            else:
                portals['port'] = split_portal[2].split(":")[1]
            portal_group_json.append({
               "params": {
                    "portals": [portals],
                    "tag": tag
                },
                "method": "add_portal_group"
            })

    return portal_group_json

def get_initiator_group_json(setup, name):
    tag = name.replace("InitiatorGroup", "")
    initiators = []
    netmasks = []
    for item in setup:
        if "InitiatorName" in item:
            initiators.append(item.split(" ")[-1])
        if "Netmask" in item:
            netmasks.append(item.split(" ")[-1])
    initiator_group_json = {
        "params": {
            "initiators": initiators,
            "tag": tag,
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
        if "BdevIoPoolSize" in item:
            bdev_json['params']['bdev_io_pool_size'] = int(item.split(" ")[-1])
        if "BdevIoCacheSize" in item:
            bdev_json['params']['bdev_io_cache_size'] = int(item.split(" ")[-1])

    return bdev_json

def get_pmem_json(setup):
    pmem_json = []
    for item in setup:
        if "Blk" in item:
            pmem_path = item.split(" ")[1]
            pmem_name = item.split(" ")[-1]
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
        if "AIO" in item:
            params = {}
            aio_split = item.split(" ")
            params['filename'] = aio_split[1]
            params['name'] = aio_split[2]
            if len(aio_split) == 4:
                params['block_size'] = aio_split[-1]
            aio_json.append({
                "params": params,
                "method": "construct_aio_bdev"
            })

    return aio_json

def get_virtio_user_json(setup, name):
    dev_type = "scsi"
    for item in setup:
        if "Path" in item:
            path = item.split(" ")[-1]
        if "Queues" in item:
            queues = item.split(" ")[-1]
        if "Type" in item:
            type = item.split(" ")[-1].lower()
            if type == "blk":
                dev_type = "blk"
        if "Name" in item:
            name = item.split(" ")[-1]

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
