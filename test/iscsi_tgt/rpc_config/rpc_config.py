#!/usr/bin/env python3


import os
import os.path
import re
import sys
import time
import json
import random
from subprocess import check_call, call, check_output, Popen, PIPE, CalledProcessError

if (len(sys.argv) == 7):
    target_ip = sys.argv[2]
    initiator_ip = sys.argv[3]
    port = sys.argv[4]
    netmask = sys.argv[5]
    namespace = sys.argv[6]

ns_cmd = 'ip netns exec ' + namespace
other_ip = '127.0.0.6'
initiator_name = 'ANY'
portal_tag = '1'
initiator_tag = '1'

rpc_param = {
    'target_ip': target_ip,
    'initiator_ip': initiator_ip,
    'port': port,
    'initiator_name': initiator_name,
    'netmask': netmask,
    'lun_total': 3,
    'malloc_bdev_size': 64,
    'malloc_block_size': 512,
    'queue_depth': 64,
    'target_name': 'Target3',
    'alias_name': 'Target3_alias',
    'disable_chap': True,
    'mutual_chap': False,
    'require_chap': False,
    'chap_group': 0,
    'header_digest': False,
    'data_digest': False,
    'log_flag': 'rpc',
    'cpumask': 0x1
}


class RpcException(Exception):

    def __init__(self, retval, msg):
        super(RpcException, self).__init__(msg)
        self.retval = retval
        self.message = msg


class spdk_rpc(object):

    def __init__(self, rpc_py):
        self.rpc_py = rpc_py

    def __getattr__(self, name):
        def call(*args):
            cmd = "{} {}".format(self.rpc_py, name)
            for arg in args:
                cmd += " {}".format(arg)
            return check_output(cmd, shell=True).decode("utf-8")
        return call


def verify(expr, retcode, msg):
    if not expr:
        raise RpcException(retcode, msg)


def verify_log_flag_rpc_methods(rpc_py, rpc_param):
    rpc = spdk_rpc(rpc_py)
    output = rpc.log_get_flags()
    jsonvalue = json.loads(output)
    verify(not jsonvalue[rpc_param['log_flag']], 1,
           "log_get_flags returned {}, expected false".format(jsonvalue))
    rpc.log_set_flag(rpc_param['log_flag'])
    output = rpc.log_get_flags()
    jsonvalue = json.loads(output)
    verify(jsonvalue[rpc_param['log_flag']], 1,
           "log_get_flags returned {}, expected true".format(jsonvalue))
    rpc.log_clear_flag(rpc_param['log_flag'])
    output = rpc.log_get_flags()
    jsonvalue = json.loads(output)
    verify(not jsonvalue[rpc_param['log_flag']], 1,
           "log_get_flags returned {}, expected false".format(jsonvalue))

    print("verify_log_flag_rpc_methods passed")


def verify_iscsi_connection_rpc_methods(rpc_py):
    rpc = spdk_rpc(rpc_py)
    output = rpc.iscsi_get_connections()
    jsonvalue = json.loads(output)
    verify(not jsonvalue, 1,
           "iscsi_get_connections returned {}, expected empty".format(jsonvalue))

    rpc.bdev_malloc_create(rpc_param['malloc_bdev_size'], rpc_param['malloc_block_size'])
    rpc.iscsi_create_portal_group(portal_tag, "{}:{}".format(rpc_param['target_ip'], str(rpc_param['port'])))
    rpc.iscsi_create_initiator_group(initiator_tag, rpc_param['initiator_name'], rpc_param['netmask'])

    lun_mapping = "Malloc" + str(rpc_param['lun_total']) + ":0"
    net_mapping = portal_tag + ":" + initiator_tag
    rpc.iscsi_create_target_node(rpc_param['target_name'], rpc_param['alias_name'], lun_mapping,
                                 net_mapping, rpc_param['queue_depth'], '-d')
    check_output('iscsiadm -m discovery -t st -p {}'.format(rpc_param['target_ip']), shell=True)
    check_output('iscsiadm -m node --login', shell=True)
    name = json.loads(rpc.iscsi_get_target_nodes())[0]['name']
    output = rpc.iscsi_get_connections()
    jsonvalues = json.loads(output)
    verify(jsonvalues[0]['target_node_name'] == rpc_param['target_name'], 1,
           "target node name vaule is {}, expected {}".format(jsonvalues[0]['target_node_name'], rpc_param['target_name']))
    verify(jsonvalues[0]['initiator_addr'] == rpc_param['initiator_ip'], 1,
           "initiator address values is {}, expected {}".format(jsonvalues[0]['initiator_addr'], rpc_param['initiator_ip']))
    verify(jsonvalues[0]['target_addr'] == rpc_param['target_ip'], 1,
           "target address values is {}, expected {}".format(jsonvalues[0]['target_addr'], rpc_param['target_ip']))

    check_output('iscsiadm -m node --logout', shell=True)
    check_output('iscsiadm -m node -o delete', shell=True)
    rpc.iscsi_delete_initiator_group(initiator_tag)
    rpc.iscsi_delete_portal_group(portal_tag)
    rpc.iscsi_delete_target_node(name)
    output = rpc.iscsi_get_connections()
    jsonvalues = json.loads(output)
    verify(not jsonvalues, 1,
           "iscsi_get_connections returned {}, expected empty".format(jsonvalues))

    print("verify_iscsi_connection_rpc_methods passed")


def verify_scsi_devices_rpc_methods(rpc_py):
    rpc = spdk_rpc(rpc_py)
    output = rpc.scsi_get_devices()
    jsonvalue = json.loads(output)
    verify(not jsonvalue, 1,
           "scsi_get_devices returned {}, expected empty".format(jsonvalue))

    rpc.bdev_malloc_create(rpc_param['malloc_bdev_size'], rpc_param['malloc_block_size'])
    rpc.iscsi_create_portal_group(portal_tag, "{}:{}".format(rpc_param['target_ip'], str(rpc_param['port'])))
    rpc.iscsi_create_initiator_group(initiator_tag, rpc_param['initiator_name'], rpc_param['netmask'])

    lun_mapping = "Malloc" + str(rpc_param['lun_total']) + ":0"
    net_mapping = portal_tag + ":" + initiator_tag
    rpc.iscsi_create_target_node(rpc_param['target_name'], rpc_param['alias_name'], lun_mapping,
                                 net_mapping, rpc_param['queue_depth'], '-d')
    check_output('iscsiadm -m discovery -t st -p {}'.format(rpc_param['target_ip']), shell=True)
    check_output('iscsiadm -m node --login', shell=True)
    name = json.loads(rpc.iscsi_get_target_nodes())[0]['name']
    output = rpc.iscsi_get_options()
    jsonvalues = json.loads(output)
    nodebase = jsonvalues['node_base']
    output = rpc.scsi_get_devices()
    jsonvalues = json.loads(output)
    verify(jsonvalues[0]['device_name'] == nodebase + ":" + rpc_param['target_name'], 1,
           "device name vaule is {}, expected {}".format(jsonvalues[0]['device_name'], rpc_param['target_name']))
    verify(jsonvalues[0]['id'] == 0, 1,
           "device id value is {}, expected 0".format(jsonvalues[0]['id']))

    check_output('iscsiadm -m node --logout', shell=True)
    check_output('iscsiadm -m node -o delete', shell=True)
    rpc.iscsi_delete_initiator_group(initiator_tag)
    rpc.iscsi_delete_portal_group(portal_tag)
    rpc.iscsi_delete_target_node(name)
    output = rpc.scsi_get_devices()
    jsonvalues = json.loads(output)
    verify(not jsonvalues, 1,
           "scsi_get_devices returned {}, expected empty".format(jsonvalues))

    print("verify_scsi_devices_rpc_methods passed")


def create_malloc_bdevs_rpc_methods(rpc_py, rpc_param):
    rpc = spdk_rpc(rpc_py)

    for i in range(1, rpc_param['lun_total'] + 1):
        rpc.bdev_malloc_create(rpc_param['malloc_bdev_size'], rpc_param['malloc_block_size'])

    print("create_malloc_bdevs_rpc_methods passed")


def verify_portal_groups_rpc_methods(rpc_py, rpc_param):
    rpc = spdk_rpc(rpc_py)
    output = rpc.iscsi_get_portal_groups()
    jsonvalues = json.loads(output)
    verify(not jsonvalues, 1,
           "iscsi_get_portal_groups returned {} groups, expected empty".format(jsonvalues))

    lo_ip = (target_ip, other_ip)
    nics = json.loads(rpc.net_get_interfaces())
    for x in nics:
        if x["ifc_index"] == 'lo':
            rpc.net_interface_add_ip_address(x["ifc_index"], lo_ip[1])
    for idx, value in enumerate(lo_ip):
        # The portal group tag must start at 1
        tag = idx + 1
        rpc.iscsi_create_portal_group(tag, "{}:{}".format(value, rpc_param['port']))
        output = rpc.iscsi_get_portal_groups()
        jsonvalues = json.loads(output)
        verify(len(jsonvalues) == tag, 1,
               "iscsi_get_portal_groups returned {} groups, expected {}".format(len(jsonvalues), tag))

    tag_list = []
    for idx, value in enumerate(jsonvalues):
        verify(value['portals'][0]['host'] == lo_ip[idx], 1,
               "host value is {}, expected {}".format(value['portals'][0]['host'], rpc_param['target_ip']))
        verify(value['portals'][0]['port'] == str(rpc_param['port']), 1,
               "port value is {}, expected {}".format(value['portals'][0]['port'], str(rpc_param['port'])))
        tag_list.append(value['tag'])
        verify(value['tag'] == idx + 1, 1,
               "tag value is {}, expected {}".format(value['tag'], idx + 1))

    for idx, value in enumerate(tag_list):
        rpc.iscsi_delete_portal_group(value)
        output = rpc.iscsi_get_portal_groups()
        jsonvalues = json.loads(output)
        verify(len(jsonvalues) == (len(tag_list) - (idx + 1)), 1,
               "get_portal_group returned {} groups, expected {}".format(len(jsonvalues), (len(tag_list) - (idx + 1))))
        if not jsonvalues:
            break

        for jidx, jvalue in enumerate(jsonvalues):
            verify(jvalue['portals'][0]['host'] == lo_ip[idx + jidx + 1], 1,
                   "host value is {}, expected {}".format(jvalue['portals'][0]['host'], lo_ip[idx + jidx + 1]))
            verify(jvalue['portals'][0]['port'] == str(rpc_param['port']), 1,
                   "port value is {}, expected {}".format(jvalue['portals'][0]['port'], str(rpc_param['port'])))
            verify(jvalue['tag'] != value or jvalue['tag'] == tag_list[idx + jidx + 1], 1,
                   "tag value is {}, expected {} and not {}".format(jvalue['tag'], tag_list[idx + jidx + 1], value))

    for x in nics:
        if x["ifc_index"] == 'lo':
            rpc.net_interface_delete_ip_address(x["ifc_index"], lo_ip[1])

    print("verify_portal_groups_rpc_methods passed")


def verify_initiator_groups_rpc_methods(rpc_py, rpc_param):
    rpc = spdk_rpc(rpc_py)
    output = rpc.iscsi_get_initiator_groups()
    jsonvalues = json.loads(output)
    verify(not jsonvalues, 1,
           "iscsi_get_initiator_groups returned {}, expected empty".format(jsonvalues))
    for idx, value in enumerate(rpc_param['netmask']):
        # The initiator group tag must start at 1
        tag = idx + 1
        rpc.iscsi_create_initiator_group(tag, rpc_param['initiator_name'], value)
        output = rpc.iscsi_get_initiator_groups()
        jsonvalues = json.loads(output)
        verify(len(jsonvalues) == tag, 1,
               "iscsi_get_initiator_groups returned {} groups, expected {}".format(len(jsonvalues), tag))

    tag_list = []
    for idx, value in enumerate(jsonvalues):
        verify(value['initiators'][0] == rpc_param['initiator_name'], 1,
               "initiator value is {}, expected {}".format(value['initiators'][0], rpc_param['initiator_name']))
        tag_list.append(value['tag'])
        verify(value['tag'] == idx + 1, 1,
               "tag value is {}, expected {}".format(value['tag'], idx + 1))
        verify(value['netmasks'][0] == rpc_param['netmask'][idx], 1,
               "netmasks value is {}, expected {}".format(value['netmasks'][0], rpc_param['netmask'][idx]))

    for idx, value in enumerate(rpc_param['netmask']):
        tag = idx + 1
        rpc.iscsi_initiator_group_remove_initiators(tag, '-n', rpc_param['initiator_name'], '-m', value)

    output = rpc.iscsi_get_initiator_groups()
    jsonvalues = json.loads(output)
    verify(len(jsonvalues) == tag, 1,
           "iscsi_get_initiator_groups returned {} groups, expected {}".format(len(jsonvalues), tag))

    for idx, value in enumerate(jsonvalues):
        verify(value['tag'] == idx + 1, 1,
               "tag value is {}, expected {}".format(value['tag'], idx + 1))
        initiators = value.get('initiators')
        verify(len(initiators) == 0, 1,
               "length of initiator list is {}, expected 0".format(len(initiators)))
        netmasks = value.get('netmasks')
        verify(len(netmasks) == 0, 1,
               "length of netmask list is {}, expected 0".format(len(netmasks)))

    for idx, value in enumerate(rpc_param['netmask']):
        tag = idx + 1
        rpc.iscsi_initiator_group_add_initiators(tag, '-n', rpc_param['initiator_name'], '-m', value)
    output = rpc.iscsi_get_initiator_groups()
    jsonvalues = json.loads(output)
    verify(len(jsonvalues) == tag, 1,
           "iscsi_get_initiator_groups returned {} groups, expected {}".format(len(jsonvalues), tag))

    tag_list = []
    for idx, value in enumerate(jsonvalues):
        verify(value['initiators'][0] == rpc_param['initiator_name'], 1,
               "initiator value is {}, expected {}".format(value['initiators'][0], rpc_param['initiator_name']))
        tag_list.append(value['tag'])
        verify(value['tag'] == idx + 1, 1,
               "tag value is {}, expected {}".format(value['tag'], idx + 1))
        verify(value['netmasks'][0] == rpc_param['netmask'][idx], 1,
               "netmasks value is {}, expected {}".format(value['netmasks'][0], rpc_param['netmask'][idx]))

    for idx, value in enumerate(tag_list):
        rpc.iscsi_delete_initiator_group(value)
        output = rpc.iscsi_get_initiator_groups()
        jsonvalues = json.loads(output)
        verify(len(jsonvalues) == (len(tag_list) - (idx + 1)), 1,
               "iscsi_get_initiator_groups returned {} groups, expected {}".format(len(jsonvalues), (len(tag_list) - (idx + 1))))
        if not jsonvalues:
            break
        for jidx, jvalue in enumerate(jsonvalues):
            verify(jvalue['initiators'][0] == rpc_param['initiator_name'], 1,
                   "initiator value is {}, expected {}".format(jvalue['initiators'][0], rpc_param['initiator_name']))
            verify(jvalue['tag'] != value or jvalue['tag'] == tag_list[idx + jidx + 1], 1,
                   "tag value is {}, expected {} and not {}".format(jvalue['tag'], tag_list[idx + jidx + 1], value))
            verify(jvalue['netmasks'][0] == rpc_param['netmask'][idx + jidx + 1], 1,
                   "netmasks value is {}, expected {}".format(jvalue['netmasks'][0], rpc_param['netmask'][idx + jidx + 1]))

    print("verify_initiator_groups_rpc_method passed.")


def verify_target_nodes_rpc_methods(rpc_py, rpc_param):
    rpc = spdk_rpc(rpc_py)
    output = rpc.iscsi_get_options()
    jsonvalues = json.loads(output)
    nodebase = jsonvalues['node_base']
    output = rpc.iscsi_get_target_nodes()
    jsonvalues = json.loads(output)
    verify(not jsonvalues, 1,
           "iscsi_get_target_nodes returned {}, expected empty".format(jsonvalues))

    rpc.bdev_malloc_create(rpc_param['malloc_bdev_size'], rpc_param['malloc_block_size'])
    rpc.iscsi_create_portal_group(portal_tag, "{}:{}".format(rpc_param['target_ip'], str(rpc_param['port'])))
    rpc.iscsi_create_initiator_group(initiator_tag, rpc_param['initiator_name'], rpc_param['netmask'])

    lun_mapping = "Malloc" + str(rpc_param['lun_total']) + ":0"
    net_mapping = portal_tag + ":" + initiator_tag
    rpc.iscsi_create_target_node(rpc_param['target_name'], rpc_param['alias_name'], lun_mapping,
                                 net_mapping, rpc_param['queue_depth'], '-d')
    output = rpc.iscsi_get_target_nodes()
    jsonvalues = json.loads(output)
    verify(len(jsonvalues) == 1, 1,
           "iscsi_get_target_nodes returned {} nodes, expected 1".format(len(jsonvalues)))
    bdev_name = jsonvalues[0]['luns'][0]['bdev_name']
    verify(bdev_name == "Malloc" + str(rpc_param['lun_total']), 1,
           "bdev_name value is {}, expected Malloc{}".format(jsonvalues[0]['luns'][0]['bdev_name'], str(rpc_param['lun_total'])))
    name = jsonvalues[0]['name']
    verify(name == nodebase + ":" + rpc_param['target_name'], 1,
           "target name value is {}, expected {}".format(name, nodebase + ":" + rpc_param['target_name']))
    verify(jsonvalues[0]['alias_name'] == rpc_param['alias_name'], 1,
           "target alias_name value is {}, expected {}".format(jsonvalues[0]['alias_name'], rpc_param['alias_name']))
    verify(jsonvalues[0]['luns'][0]['lun_id'] == 0, 1,
           "lun id value is {}, expected 0".format(jsonvalues[0]['luns'][0]['lun_id']))
    verify(jsonvalues[0]['pg_ig_maps'][0]['ig_tag'] == int(initiator_tag), 1,
           "initiator group tag value is {}, expected {}".format(jsonvalues[0]['pg_ig_maps'][0]['ig_tag'], initiator_tag))
    verify(jsonvalues[0]['queue_depth'] == rpc_param['queue_depth'], 1,
           "queue depth value is {}, expected {}".format(jsonvalues[0]['queue_depth'], rpc_param['queue_depth']))
    verify(jsonvalues[0]['pg_ig_maps'][0]['pg_tag'] == int(portal_tag), 1,
           "portal group tag value is {}, expected {}".format(jsonvalues[0]['pg_ig_maps'][0]['pg_tag'], portal_tag))
    verify(jsonvalues[0]['disable_chap'] == rpc_param['disable_chap'], 1,
           "disable chap value is {}, expected {}".format(jsonvalues[0]['disable_chap'], rpc_param['disable_chap']))
    verify(jsonvalues[0]['mutual_chap'] == rpc_param['mutual_chap'], 1,
           "chap mutual value is {}, expected {}".format(jsonvalues[0]['mutual_chap'], rpc_param['mutual_chap']))
    verify(jsonvalues[0]['require_chap'] == rpc_param['require_chap'], 1,
           "chap required value is {}, expected {}".format(jsonvalues[0]['require_chap'], rpc_param['require_chap']))
    verify(jsonvalues[0]['chap_group'] == rpc_param['chap_group'], 1,
           "chap auth group value is {}, expected {}".format(jsonvalues[0]['chap_group'], rpc_param['chap_group']))
    verify(jsonvalues[0]['header_digest'] == rpc_param['header_digest'], 1,
           "header digest value is {}, expected {}".format(jsonvalues[0]['header_digest'], rpc_param['header_digest']))
    verify(jsonvalues[0]['data_digest'] == rpc_param['data_digest'], 1,
           "data digest value is {}, expected {}".format(jsonvalues[0]['data_digest'], rpc_param['data_digest']))
    lun_id = '1'
    rpc.iscsi_target_node_add_lun(name, bdev_name, "-i", lun_id)
    output = rpc.iscsi_get_target_nodes()
    jsonvalues = json.loads(output)
    verify(jsonvalues[0]['luns'][1]['bdev_name'] == "Malloc" + str(rpc_param['lun_total']), 1,
           "bdev_name value is {}, expected Malloc{}".format(jsonvalues[0]['luns'][0]['bdev_name'], str(rpc_param['lun_total'])))
    verify(jsonvalues[0]['luns'][1]['lun_id'] == 1, 1,
           "lun id value is {}, expected 1".format(jsonvalues[0]['luns'][1]['lun_id']))

    rpc.iscsi_delete_target_node(name)
    output = rpc.iscsi_get_target_nodes()
    jsonvalues = json.loads(output)
    verify(not jsonvalues, 1,
           "iscsi_get_target_nodes returned {}, expected empty".format(jsonvalues))

    rpc.iscsi_create_target_node(rpc_param['target_name'], rpc_param['alias_name'], lun_mapping,
                                 net_mapping, rpc_param['queue_depth'], '-d')

    rpc.iscsi_delete_portal_group(portal_tag)
    rpc.iscsi_delete_initiator_group(initiator_tag)
    rpc.iscsi_delete_target_node(name)
    output = rpc.iscsi_get_target_nodes()
    jsonvalues = json.loads(output)
    if not jsonvalues:
        print("This issue will be fixed later.")

    print("verify_target_nodes_rpc_methods passed.")


def verify_net_get_interfaces(rpc_py):
    rpc = spdk_rpc(rpc_py)
    nics = json.loads(rpc.net_get_interfaces())
    nics_names = set(x["name"] for x in nics)
    # parse ip link show to verify the net_get_interfaces result
    ip_show = ns_cmd + " ip link show"
    ifcfg_nics = set(re.findall(r'\S+:\s(\S+?)(?:@\S+){0,1}:\s<.*', check_output(ip_show.split()).decode()))
    verify(nics_names == ifcfg_nics, 1, "net_get_interfaces returned {}".format(nics))
    print("verify_net_get_interfaces passed.")


def help_get_interface_ip_list(rpc_py, nic_name):
    rpc = spdk_rpc(rpc_py)
    nics = json.loads(rpc.net_get_interfaces())
    nic = list([x for x in nics if x["name"] == nic_name])
    verify(len(nic) != 0, 1,
           "Nic name: {} is not found in {}".format(nic_name, [x["name"] for x in nics]))
    return nic[0]["ip_addr"]


def verify_net_interface_add_delete_ip_address(rpc_py):
    rpc = spdk_rpc(rpc_py)
    nics = json.loads(rpc.net_get_interfaces())
    # add ip on up to first 2 nics
    for x in nics[:2]:
        faked_ip = "123.123.{}.{}".format(random.randint(1, 254), random.randint(1, 254))
        ping_cmd = ns_cmd + " ping -c 1 -W 1 " + faked_ip
        rpc.net_interface_add_ip_address(x["ifc_index"], faked_ip)
        verify(faked_ip in help_get_interface_ip_list(rpc_py, x["name"]), 1,
               "add ip {} to nic {} failed.".format(faked_ip, x["name"]))
        try:
            check_call(ping_cmd.split())
        except BaseException:
            verify(False, 1,
                   "ping ip {} for {} was failed(adding was successful)".format
                   (faked_ip, x["name"]))
        rpc.net_interface_delete_ip_address(x["ifc_index"], faked_ip)
        verify(faked_ip not in help_get_interface_ip_list(rpc_py, x["name"]), 1,
               "delete ip {} from nic {} failed.(adding and ping were successful)".format
               (faked_ip, x["name"]))
        # ping should be failed and throw an CalledProcessError exception
        try:
            check_call(ping_cmd.split())
        except CalledProcessError as _:
            pass
        except Exception as e:
            verify(False, 1,
                   "Unexpected exception was caught {}(adding/ping/delete were successful)".format
                   (str(e)))
        else:
            verify(False, 1,
                   "ip {} for {} could be pinged after delete ip(adding/ping/delete were successful)".format
                   (faked_ip, x["name"]))
    print("verify_net_interface_add_delete_ip_address passed.")


if __name__ == "__main__":

    rpc_py = sys.argv[1]

    try:
        verify_log_flag_rpc_methods(rpc_py, rpc_param)
        verify_net_get_interfaces(rpc_py)
        verify_net_interface_add_delete_ip_address(rpc_py)
        create_malloc_bdevs_rpc_methods(rpc_py, rpc_param)
        verify_portal_groups_rpc_methods(rpc_py, rpc_param)
        verify_initiator_groups_rpc_methods(rpc_py, rpc_param)
        verify_target_nodes_rpc_methods(rpc_py, rpc_param)
        verify_scsi_devices_rpc_methods(rpc_py)
        verify_iscsi_connection_rpc_methods(rpc_py)
    except RpcException as e:
        print("{}. Exiting with status {}".format(e.message, e.retval))
        raise e
    except Exception as e:
        raise e

    sys.exit(0)
