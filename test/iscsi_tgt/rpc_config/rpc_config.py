#!/usr/bin/env python


import os
import os.path
import re
import sys
import time
import json
import random
from subprocess import check_call, call, check_output, Popen, PIPE, CalledProcessError

netmask = ('127.0.0.1', '127.0.0.0')
rpc_param = {
    'target_ip': '127.0.0.1',
    'port': 3260,
    'initiator_name': 'ANY',
    'netmask': netmask,
    'lun_total': 3,
    'malloc_bdev_size': 64,
    'malloc_block_size': 512,
    'queue_depth': 64,
    'target_name': 'Target3',
    'alias_name': 'Target3_alias',
    'chap_disable': 1,
    'chap_mutal': 0,
    'chap_required': 0,
    'chap_auth_group': 0,
    'header_digest': 0,
    'data_digest': 0,
    'trace_flag': 'rpc',
    'cpumask': 0x1
}


class RpcException(Exception):

    def __init__(self, retval, *args):
        super(RpcException, self).__init__(*args)
        self.retval = retval


class spdk_rpc(object):

    def __init__(self, rpc_py):
        self.rpc_py = rpc_py

    def __getattr__(self, name):
        def call(*args):
            cmd = "python {} {}".format(self.rpc_py, name)
            for arg in args:
                cmd += " {}".format(arg)
            return check_output(cmd, shell=True)
        return call


def verify(expr, retcode, msg):
    if not expr:
        raise RpcException(retcode, msg)


def verify_trace_flag_rpc_methods(rpc_py, rpc_param):
    rpc = spdk_rpc(rpc_py)
    output = rpc.get_trace_flags()
    jsonvalue = json.loads(output)
    verify(not jsonvalue[rpc_param['trace_flag']], 1,
           "get_trace_flags returned {}, expected false".format(jsonvalue))
    rpc.set_trace_flag(rpc_param['trace_flag'])
    output = rpc.get_trace_flags()
    jsonvalue = json.loads(output)
    verify(jsonvalue[rpc_param['trace_flag']], 1,
           "get_trace_flags returned {}, expected true".format(jsonvalue))
    rpc.clear_trace_flag(rpc_param['trace_flag'])
    output = rpc.get_trace_flags()
    jsonvalue = json.loads(output)
    verify(not jsonvalue[rpc_param['trace_flag']], 1,
           "get_trace_flags returned {}, expected false".format(jsonvalue))

    print "verify_trace_flag_rpc_methods passed"


def verify_iscsi_connection_rpc_methods(rpc_py):
    rpc = spdk_rpc(rpc_py)
    output = rpc.get_iscsi_connections()
    jsonvalue = json.loads(output)
    verify(not jsonvalue, 1,
           "get_iscsi_connections returned {}, expected empty".format(jsonvalue))

    portal_tag = '1'
    initiator_tag = '1'
    rpc.construct_malloc_bdev(rpc_param['malloc_bdev_size'], rpc_param['malloc_block_size'])
    rpc.add_portal_group(portal_tag, "{}:{}".format(rpc_param['target_ip'], str(rpc_param['port'])))
    rpc.add_initiator_group(initiator_tag, rpc_param['initiator_name'], rpc_param['netmask'][0])

    lun_mapping = "Malloc" + str(rpc_param['lun_total']) + ":0"
    net_mapping = portal_tag + ":" + initiator_tag
    rpc.construct_target_node(rpc_param['target_name'], rpc_param['alias_name'], lun_mapping, net_mapping, rpc_param['queue_depth'],
                              rpc_param['chap_disable'], rpc_param['chap_mutal'], rpc_param['chap_required'], rpc_param['chap_auth_group'])
    check_output('iscsiadm -m discovery -t st -p {}'.format(rpc_param['target_ip']), shell=True)
    check_output('iscsiadm -m node --login', shell=True)
    name = json.loads(rpc.get_target_nodes())[0]['name']
    output = rpc.get_iscsi_connections()
    jsonvalues = json.loads(output)
    verify(jsonvalues[0]['target_node_name'] == rpc_param['target_name'], 1,
           "target node name vaule is {}, expected {}".format(jsonvalues[0]['target_node_name'], rpc_param['target_name']))
    verify(jsonvalues[0]['id'] == 0, 1,
           "device id value is {}, expected 0".format(jsonvalues[0]['id']))
    verify(jsonvalues[0]['initiator_addr'] == rpc_param['target_ip'], 1,
           "initiator address values is {}, expected {}".format(jsonvalues[0]['initiator_addr'], rpc_param['target_ip']))
    verify(jsonvalues[0]['target_addr'] == rpc_param['target_ip'], 1,
           "target address values is {}, expected {}".format(jsonvalues[0]['target_addr'], rpc_param['target_ip']))

    check_output('iscsiadm -m node --logout', shell=True)
    check_output('iscsiadm -m node -o delete', shell=True)
    rpc.delete_initiator_group(initiator_tag)
    rpc.delete_portal_group(portal_tag)
    rpc.delete_target_node(name)
    output = rpc.get_iscsi_connections()
    jsonvalues = json.loads(output)
    verify(not jsonvalues, 1,
           "get_iscsi_connections returned {}, expected empty".format(jsonvalues))

    print "verify_iscsi_connection_rpc_methods passed"


def verify_scsi_devices_rpc_methods(rpc_py):
    rpc = spdk_rpc(rpc_py)
    output = rpc.get_scsi_devices()
    jsonvalue = json.loads(output)
    verify(not jsonvalue, 1,
           "get_scsi_devices returned {}, expected empty".format(jsonvalue))

    portal_tag = '1'
    initiator_tag = '1'
    rpc.construct_malloc_bdev(rpc_param['malloc_bdev_size'], rpc_param['malloc_block_size'])
    rpc.add_portal_group(portal_tag, "{}:{}".format(rpc_param['target_ip'], str(rpc_param['port'])))
    rpc.add_initiator_group(initiator_tag, rpc_param['initiator_name'], rpc_param['netmask'][0])

    lun_mapping = "Malloc" + str(rpc_param['lun_total']) + ":0"
    net_mapping = portal_tag + ":" + initiator_tag
    rpc.construct_target_node(rpc_param['target_name'], rpc_param['alias_name'], lun_mapping, net_mapping, rpc_param['queue_depth'],
                              rpc_param['chap_disable'], rpc_param['chap_mutal'], rpc_param['chap_required'], rpc_param['chap_auth_group'])
    check_output('iscsiadm -m discovery -t st -p {}'.format(rpc_param['target_ip']), shell=True)
    check_output('iscsiadm -m node --login', shell=True)
    name = json.loads(rpc.get_target_nodes())[0]['name']
    output = rpc.get_iscsi_global_params()
    jsonvalues = json.loads(output)
    nodebase = jsonvalues['node_base']
    output = rpc.get_scsi_devices()
    jsonvalues = json.loads(output)
    verify(jsonvalues[0]['device_name'] == nodebase + ":" + rpc_param['target_name'], 1,
           "device name vaule is {}, expected {}".format(jsonvalues[0]['device_name'], rpc_param['target_name']))
    verify(jsonvalues[0]['id'] == 0, 1,
           "device id value is {}, expected 0".format(jsonvalues[0]['id']))

    check_output('iscsiadm -m node --logout', shell=True)
    check_output('iscsiadm -m node -o delete', shell=True)
    rpc.delete_initiator_group(initiator_tag)
    rpc.delete_portal_group(portal_tag)
    rpc.delete_target_node(name)
    output = rpc.get_scsi_devices()
    jsonvalues = json.loads(output)
    verify(not jsonvalues, 1,
           "get_scsi_devices returned {}, expected empty".format(jsonvalues))

    print "verify_scsi_devices_rpc_methods passed"


def create_malloc_bdevs_rpc_methods(rpc_py, rpc_param):
    rpc = spdk_rpc(rpc_py)

    for i in range(1, rpc_param['lun_total'] + 1):
        rpc.construct_malloc_bdev(rpc_param['malloc_bdev_size'], rpc_param['malloc_block_size'])

    print "create_malloc_bdevs_rpc_methods passed"


def verify_portal_groups_rpc_methods(rpc_py, rpc_param):
    rpc = spdk_rpc(rpc_py)
    output = rpc.get_portal_groups()
    jsonvalues = json.loads(output)
    verify(not jsonvalues, 1,
           "get_portal_groups returned {} groups, expected empty".format(jsonvalues))

    lo_ip = ('127.0.0.1', '127.0.0.6')
    nics = json.loads(rpc.get_interfaces())
    for x in nics:
        if x["ifc_index"] == 'lo':
            rpc.add_ip_address(x["ifc_index"], lo_ip[1])
    for idx, value in enumerate(lo_ip):
        # The portal group tag must start at 1
        tag = idx + 1
        rpc.add_portal_group(tag, "{}:{}@{}".format(value, rpc_param['port'], rpc_param['cpumask']))
        output = rpc.get_portal_groups()
        jsonvalues = json.loads(output)
        verify(len(jsonvalues) == tag, 1,
               "get_portal_groups returned {} groups, expected {}".format(len(jsonvalues), tag))

    tag_list = []
    for idx, value in enumerate(jsonvalues):
        verify(value['portals'][0]['host'] == lo_ip[idx], 1,
               "host value is {}, expected {}".format(value['portals'][0]['host'], rpc_param['target_ip']))
        verify(value['portals'][0]['port'] == str(rpc_param['port']), 1,
               "port value is {}, expected {}".format(value['portals'][0]['port'], str(rpc_param['port'])))
        verify(value['portals'][0]['cpumask'] == format(rpc_param['cpumask'], '#x'), 1,
               "cpumask value is {}, expected {}".format(value['portals'][0]['cpumask'], format(rpc_param['cpumask'], '#x')))
        tag_list.append(value['tag'])
        verify(value['tag'] == idx + 1, 1,
               "tag value is {}, expected {}".format(value['tag'], idx + 1))

    for idx, value in enumerate(tag_list):
        rpc.delete_portal_group(value)
        output = rpc.get_portal_groups()
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
            verify(jvalue['portals'][0]['cpumask'] == format(rpc_param['cpumask'], '#x'), 1,
                   "cpumask value is {}, expected {}".format(jvalue['portals'][0]['cpumask'], format(rpc_param['cpumask'], '#x')))
            verify(jvalue['tag'] != value or jvalue['tag'] == tag_list[idx + jidx + 1], 1,
                   "tag value is {}, expected {} and not {}".format(jvalue['tag'], tag_list[idx + jidx + 1], value))

    for x in nics:
        if x["ifc_index"] == 'lo':
            rpc.delete_ip_address(x["ifc_index"], lo_ip[1])

    print "verify_portal_groups_rpc_methods passed"


def verify_initiator_groups_rpc_methods(rpc_py, rpc_param):
    rpc = spdk_rpc(rpc_py)
    output = rpc.get_initiator_groups()
    jsonvalues = json.loads(output)
    verify(not jsonvalues, 1,
           "get_initiator_groups returned {}, expected empty".format(jsonvalues))
    for idx, value in enumerate(rpc_param['netmask']):
        # The initiator group tag must start at 1
        tag = idx + 1
        rpc.add_initiator_group(tag, rpc_param['initiator_name'], value)
        output = rpc.get_initiator_groups()
        jsonvalues = json.loads(output)
        verify(len(jsonvalues) == tag, 1,
               "get_initiator_groups returned {} groups, expected {}".format(len(jsonvalues), tag))

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
        rpc.delete_initiators_from_initiator_group(tag, '-n', rpc_param['initiator_name'], '-m', value)

    output = rpc.get_initiator_groups()
    jsonvalues = json.loads(output)
    verify(len(jsonvalues) == tag, 1,
           "get_initiator_groups returned {} groups, expected {}".format(len(jsonvalues), tag))

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
        rpc.add_initiators_to_initiator_group(tag, '-n', rpc_param['initiator_name'], '-m', value)
    output = rpc.get_initiator_groups()
    jsonvalues = json.loads(output)
    verify(len(jsonvalues) == tag, 1,
           "get_initiator_groups returned {} groups, expected {}".format(len(jsonvalues), tag))

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
        rpc.delete_initiator_group(value)
        output = rpc.get_initiator_groups()
        jsonvalues = json.loads(output)
        verify(len(jsonvalues) == (len(tag_list) - (idx + 1)), 1,
               "get_initiator_groups returned {} groups, expected {}".format(len(jsonvalues), (len(tag_list) - (idx + 1))))
        if not jsonvalues:
            break
        for jidx, jvalue in enumerate(jsonvalues):
            verify(jvalue['initiators'][0] == rpc_param['initiator_name'], 1,
                   "initiator value is {}, expected {}".format(jvalue['initiators'][0], rpc_param['initiator_name']))
            verify(jvalue['tag'] != value or jvalue['tag'] == tag_list[idx + jidx + 1], 1,
                   "tag value is {}, expected {} and not {}".format(jvalue['tag'], tag_list[idx + jidx + 1], value))
            verify(jvalue['netmasks'][0] == rpc_param['netmask'][idx + jidx + 1], 1,
                   "netmasks value is {}, expected {}".format(jvalue['netmasks'][0], rpc_param['netmask'][idx + jidx + 1]))

    print "verify_initiator_groups_rpc_method passed."


def verify_target_nodes_rpc_methods(rpc_py, rpc_param):
    rpc = spdk_rpc(rpc_py)
    portal_tag = '1'
    initiator_tag = '1'
    output = rpc.get_iscsi_global_params()
    jsonvalues = json.loads(output)
    nodebase = jsonvalues['node_base']
    output = rpc.get_target_nodes()
    jsonvalues = json.loads(output)
    verify(not jsonvalues, 1,
           "get_target_nodes returned {}, expected empty".format(jsonvalues))

    rpc.construct_malloc_bdev(rpc_param['malloc_bdev_size'], rpc_param['malloc_block_size'])
    rpc.add_portal_group(portal_tag, "{}:{}".format(rpc_param['target_ip'], str(rpc_param['port'])))
    rpc.add_initiator_group(initiator_tag, rpc_param['initiator_name'], rpc_param['netmask'][0])

    lun_mapping = "Malloc" + str(rpc_param['lun_total']) + ":0"
    net_mapping = portal_tag + ":" + initiator_tag
    rpc.construct_target_node(rpc_param['target_name'], rpc_param['alias_name'], lun_mapping, net_mapping, rpc_param['queue_depth'],
                              rpc_param['chap_disable'], rpc_param['chap_mutal'], rpc_param['chap_required'], rpc_param['chap_auth_group'],
                              "-H", rpc_param['header_digest'], "-D", rpc_param['data_digest'])
    output = rpc.get_target_nodes()
    jsonvalues = json.loads(output)
    verify(len(jsonvalues) == 1, 1,
           "get_target_nodes returned {} nodes, expected 1".format(len(jsonvalues)))
    bdev_name = jsonvalues[0]['luns'][0]['bdev_name']
    verify(bdev_name == "Malloc" + str(rpc_param['lun_total']), 1,
           "bdev_name value is {}, expected Malloc{}".format(jsonvalues[0]['luns'][0]['bdev_name'], str(rpc_param['lun_total'])))
    name = jsonvalues[0]['name']
    verify(name == nodebase + ":" + rpc_param['target_name'], 1,
           "target name value is {}, expected {}".format(name, nodebase + ":" + rpc_param['target_name']))
    verify(jsonvalues[0]['alias_name'] == rpc_param['alias_name'], 1,
           "target alias_name value is {}, expected {}".format(jsonvalues[0]['alias_name'], rpc_param['alias_name']))
    verify(jsonvalues[0]['luns'][0]['id'] == 0, 1,
           "lun id value is {}, expected 0".format(jsonvalues[0]['luns'][0]['id']))
    verify(jsonvalues[0]['pg_ig_maps'][0]['ig_tag'] == int(initiator_tag), 1,
           "initiator group tag value is {}, expected {}".format(jsonvalues[0]['pg_ig_maps'][0]['ig_tag'], initiator_tag))
    verify(jsonvalues[0]['queue_depth'] == rpc_param['queue_depth'], 1,
           "queue depth value is {}, expected {}".format(jsonvalues[0]['queue_depth'], rpc_param['queue_depth']))
    verify(jsonvalues[0]['pg_ig_maps'][0]['pg_tag'] == int(portal_tag), 1,
           "portal group tag value is {}, expected {}".format(jsonvalues[0]['pg_ig_maps'][0]['pg_tag'], portal_tag))
    verify(jsonvalues[0]['chap_disabled'] == rpc_param['chap_disable'], 1,
           "chap disable value is {}, expected {}".format(jsonvalues[0]['chap_disabled'], rpc_param['chap_disable']))
    verify(jsonvalues[0]['chap_mutual'] == rpc_param['chap_mutal'], 1,
           "chap mutual value is {}, expected {}".format(jsonvalues[0]['chap_mutual'], rpc_param['chap_mutal']))
    verify(jsonvalues[0]['chap_required'] == rpc_param['chap_required'], 1,
           "chap required value is {}, expected {}".format(jsonvalues[0]['chap_required'], rpc_param['chap_required']))
    verify(jsonvalues[0]['chap_auth_group'] == rpc_param['chap_auth_group'], 1,
           "chap auth group value is {}, expected {}".format(jsonvalues[0]['chap_auth_group'], rpc_param['chap_auth_group']))
    verify(jsonvalues[0]['header_digest'] == rpc_param['header_digest'], 1,
           "header digest value is {}, expected {}".format(jsonvalues[0]['header_digest'], rpc_param['header_digest']))
    verify(jsonvalues[0]['data_digest'] == rpc_param['data_digest'], 1,
           "data digest value is {}, expected {}".format(jsonvalues[0]['data_digest'], rpc_param['data_digest']))
    lun_id = '1'
    rpc.target_node_add_lun(name, bdev_name, "-i", lun_id)
    output = rpc.get_target_nodes()
    jsonvalues = json.loads(output)
    verify(jsonvalues[0]['luns'][1]['bdev_name'] == "Malloc" + str(rpc_param['lun_total']), 1,
           "bdev_name value is {}, expected Malloc{}".format(jsonvalues[0]['luns'][0]['bdev_name'], str(rpc_param['lun_total'])))
    verify(jsonvalues[0]['luns'][1]['id'] == 1, 1,
           "lun id value is {}, expected 1".format(jsonvalues[0]['luns'][1]['id']))

    rpc.delete_target_node(name)
    output = rpc.get_target_nodes()
    jsonvalues = json.loads(output)
    verify(not jsonvalues, 1,
           "get_target_nodes returned {}, expected empty".format(jsonvalues))

    rpc.construct_target_node(rpc_param['target_name'], rpc_param['alias_name'], lun_mapping, net_mapping, rpc_param['queue_depth'],
                              rpc_param['chap_disable'], rpc_param['chap_mutal'], rpc_param['chap_required'], rpc_param['chap_auth_group'])

    rpc.delete_portal_group(portal_tag)
    rpc.delete_initiator_group(initiator_tag)
    rpc.delete_target_node(name)
    output = rpc.get_target_nodes()
    jsonvalues = json.loads(output)
    if not jsonvalues:
        print "This issue will be fixed later."

    print "verify_target_nodes_rpc_methods passed."


def verify_get_interfaces(rpc_py):
    rpc = spdk_rpc(rpc_py)
    nics = json.loads(rpc.get_interfaces())
    nics_names = set(x["name"].encode('ascii', 'ignore') for x in nics)
    # parse ip link show to verify the get_interfaces result
    ifcfg_nics = set(re.findall("\S+:\s(\S+):\s<.*", check_output(["ip", "link", "show"])))
    verify(nics_names == ifcfg_nics, 1, "get_interfaces returned {}".format(nics))
    print "verify_get_interfaces passed."


def help_get_interface_ip_list(rpc_py, nic_name):
    rpc = spdk_rpc(rpc_py)
    nics = json.loads(rpc.get_interfaces())
    nic = filter(lambda x: x["name"] == nic_name, nics)
    verify(len(nic) != 0, 1,
           "Nic name: {} is not found in {}".format(nic_name, [x["name"] for x in nics]))
    return nic[0]["ip_addr"]


def verify_add_delete_ip_address(rpc_py):
    rpc = spdk_rpc(rpc_py)
    nics = json.loads(rpc.get_interfaces())
    # add ip on up to first 2 nics
    for x in nics[:2]:
        faked_ip = "123.123.{}.{}".format(random.randint(1, 254), random.randint(1, 254))
        rpc.add_ip_address(x["ifc_index"], faked_ip)
        verify(faked_ip in help_get_interface_ip_list(rpc_py, x["name"]), 1,
               "add ip {} to nic {} failed.".format(faked_ip, x["name"]))
        try:
            check_call(["ping", "-c", "1", "-W", "1", faked_ip])
        except:
            verify(False, 1,
                   "ping ip {} for {} was failed(adding was successful)".format
                   (faked_ip, x["name"]))
        rpc.delete_ip_address(x["ifc_index"], faked_ip)
        verify(faked_ip not in help_get_interface_ip_list(rpc_py, x["name"]), 1,
               "delete ip {} from nic {} failed.(adding and ping were successful)".format
               (faked_ip, x["name"]))
        # ping should be failed and throw an CalledProcessError exception
        try:
            check_call(["ping", "-c", "1", "-W", "1", faked_ip])
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
    print "verify_add_delete_ip_address passed."


def verify_add_nvme_bdev_rpc_methods(rpc_py):
    rpc = spdk_rpc(rpc_py)
    test_pass = 0
    output = check_output(["lspci", "-mm", "-nn"])
    addrs = re.findall('^([0-9]{2}:[0-9]{2}.[0-9]) "Non-Volatile memory controller \[0108\]".*-p02', output, re.MULTILINE)
    for addr in addrs:
        ctrlr_address = "-b Nvme0 -t pcie -a 0000:{}".format(addr)
        rpc.construct_nvme_bdev(ctrlr_address)
        print "add nvme device passed first time"
        test_pass = 0
        try:
            rpc.construct_nvme_bdev(ctrlr_address)
        except Exception as e:
            print "add nvme device passed second time"
            test_pass = 1
            pass
        else:
            pass
        verify(test_pass == 1, 1, "add nvme device passed second time")
    print "verify_add_nvme_bdev_rpc_methods passed."


if __name__ == "__main__":

    rpc_py = sys.argv[1]

    try:
        verify_trace_flag_rpc_methods(rpc_py, rpc_param)
        verify_get_interfaces(rpc_py)
        verify_add_delete_ip_address(rpc_py)
        create_malloc_bdevs_rpc_methods(rpc_py, rpc_param)
        verify_portal_groups_rpc_methods(rpc_py, rpc_param)
        verify_initiator_groups_rpc_methods(rpc_py, rpc_param)
        verify_target_nodes_rpc_methods(rpc_py, rpc_param)
        verify_scsi_devices_rpc_methods(rpc_py)
        verify_iscsi_connection_rpc_methods(rpc_py)
        verify_add_nvme_bdev_rpc_methods(rpc_py)
    except RpcException as e:
        print "{}. Exiting with status {}".format(e.message, e.retval)
        raise e
    except Exception as e:
        raise e

    sys.exit(0)
