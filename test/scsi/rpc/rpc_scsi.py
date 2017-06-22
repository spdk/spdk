#!/usr/bin/env python

import json
from subprocess import check_output
import sys
import re

rpc_param = {
    'malloc_bdev_size': 64,
    'malloc_block_size': 512,
    'ctrlr': 'vhost.0',
    'scsi_devs': '[]',
    'cpu_mask': "0x1",
    'scsi_dev_num': 0,
    'claimed': 'False'
}

rpc_param_second = {
    'malloc_bdev_size': 64,
    'malloc_block_size': 512,
    'ctrlr': 'vhost.0',
    'cpu_mask': "0x1",
    'scsi_dev_num': 0,
    'claimed': 'True'
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

def verify_get_bdevs_methods_null(rpc_py):
    rpc = spdk_rpc(rpc_py)
    output = rpc.get_bdevs()
    jsonvalue = json.loads(output)
    verify(not jsonvalue, 1, "get_bdevs returned:\n {}, \n expected empty".format(jsonvalue))

    print "verify_get_bdevs_null_methods passed"

def verify_construct_malloc_bdevs(rpc_py, rpc_param):
    rpc = spdk_rpc(rpc_py)
    output = rpc.construct_malloc_bdev(rpc_param['malloc_bdev_size'], rpc_param['malloc_block_size'])
    verify(re.search('^Malloc[0-9]*', output), 1, "construct_malloc_bdev returned {}, expected Malloc_".format(output))

    print "verify_construct_malloc_bdevs_methods passed"
    return output

def verify_get_bdevs_methods(rpc_py, rpc_param):
    rpc = spdk_rpc(rpc_py)
    output = rpc.get_bdevs()
    jsonvalue = json.loads(output)
    verify([str(jsonvalue[0]['block_size']) in str(rpc_param['malloc_block_size'])
            and jsonvalue[0]['claimed'] is rpc_param['claimed']], 1, "get_bdevs returned not correct values")

    print "verify_get_bdevs_methods passed"

def verify_construct_vhost_scsi_controller(rpc_py, rpc_param):
    rpc = spdk_rpc(rpc_py)
    output = rpc.construct_vhost_scsi_controller(rpc_param['ctrlr'])
    verify(not output, 1, "construct_vhost_scsi_controller returned:\n {}, \n expected empty".format(output))

    print "verify_get_bdevs_methods passed"

def verify_get_vhost_scsi_controllers(rpc_py, rpc_param, malloc):
    rpc = spdk_rpc(rpc_py)
    output = rpc.get_vhost_scsi_controllers()
    jsonvalue = json.loads(output)

    verify(jsonvalue[0]['ctrlr'] in rpc_param['ctrlr'], 1, "get_vhost_scsi_controllers cpu_mask parameter is "
        "different for value {}".format(rpc_param['ctrlr']) )

    if jsonvalue[0]['scsi_devs']:
       verify(jsonvalue[0]['scsi_devs'][0]['luns'][0]['name'] in malloc, 1, "get_vhost_scsi_controllers "
                "scsi_devs parameter is different for value {}".format(malloc))
    else:
        verify( str(jsonvalue[0]['scsi_devs']) in rpc_param['scsi_devs'], 1, "get_vhost_scsi_controllers scsi_devs "
            "parameter is different for value {}".format(rpc_param['scsi_devs']))

    verify(jsonvalue[0]['cpu_mask'] in rpc_param['cpu_mask'], 1, "get_vhost_scsi_controllers cpu_mask parameter is "
        "different for value {}".format(rpc_param['cpu_mask']))

    print "verify_get_vhost_scsi_controllers_methods passed"

def verify_add_vhost_scsi_lun(rpc_py, rpc_param, malloc):
    rpc = spdk_rpc(rpc_py)
    output = rpc.add_vhost_scsi_lun(rpc_param['ctrlr'], rpc_param['scsi_dev_num'], malloc)
    verify(not output, 1, "construct_vhost_scsi_controller returned:\n {}, \n expected empty".format(output))

    print "verify_add_vhost_scsi_lun_methods passed"

def verify_remove_vhost_scsi_dev(rpc_py, rpc_param):
    rpc = spdk_rpc(rpc_py)
    output = rpc.remove_vhost_scsi_dev(rpc_param['ctrlr'], rpc_param['scsi_dev_num'])
    verify(not output, 1, "remove_vhost_scsi_dev returned:\n {}, \n expected empty".format(output))

    print "verify_remove_vhost_scsi_dev_methods passed"

def verify_remove_vhost_scsi_controller(rpc_py, rpc_param):
    rpc = spdk_rpc(rpc_py)
    output = rpc.remove_vhost_scsi_controller(rpc_param['ctrlr'])
    verify(not output, 1, "remove_vhost_scsi_dev returned:\n {}, \n expected empty".format(output))

    print "verify_remove_vhost_scsi_controller_methods passed"

def verify_delete_bdev(rpc_py, malloc):
    rpc = spdk_rpc(rpc_py)
    output = rpc.delete_bdev(malloc)
    verify(not output, 1, "delete_bdev returned:\n {}, \n expected empty".format(output))

    print "verify_delete_bdev_methods passed"

if __name__ == "__main__":

    rpc_py = sys.argv[1]

    try:
        verify_get_bdevs_methods_null(rpc_py)
        malloc = verify_construct_malloc_bdevs(rpc_py, rpc_param)
        verify_get_bdevs_methods(rpc_py, rpc_param)
        verify_construct_vhost_scsi_controller(rpc_py, rpc_param)
        verify_get_vhost_scsi_controllers(rpc_py, rpc_param, malloc)
        verify_get_bdevs_methods(rpc_py, rpc_param_second)
        verify_add_vhost_scsi_lun(rpc_py,rpc_param, malloc)
        verify_get_vhost_scsi_controllers(rpc_py, rpc_param_second, malloc)
        verify_remove_vhost_scsi_dev(rpc_py, rpc_param)
        verify_get_vhost_scsi_controllers(rpc_py, rpc_param, malloc)
        verify_remove_vhost_scsi_controller(rpc_py, rpc_param)
        verify_get_bdevs_methods(rpc_py, rpc_param_second)
        verify_delete_bdev(rpc_py, malloc)
        verify_get_bdevs_methods_null(rpc_py)

    except RpcException as e:
        print "{}. Exiting with status {}".format(e.message, e.retval)
        raise e
    except Exception as e:
        raise e

    sys.exit(0)
