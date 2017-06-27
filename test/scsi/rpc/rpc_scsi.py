#!/usr/bin/env python

import json
import linecache
from subprocess import check_output
import sys
import re
from types import *

rpc_param = {
    'malloc_bdev_size': 64,
    'malloc_block_size': 512,
    'ctrlr': unicode('vhost.0'),
    'scsi_devs': [],
    'cpu_mask': unicode('0x1'),
    'scsi_dev_num': 0,
    'claimed': False
}

rpc_param_second = {
    'malloc_bdev_size': 64,
    'malloc_block_size': 512,
    'ctrlr': unicode('vhost.0'),
    'cpu_mask': unicode('0x1'),
    'scsi_dev_num': 0,
    'claimed': True
}


class RpcException(Exception):

    def __init__(self, expect_value, return_value, *args):
        super(RpcException, self).__init__(*args)
        self.retval = return_value
        self.exprval = expect_value


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


def print_error(expect_value, return_value):
    exc_type, exc_obj, tb = sys.exc_info()
    f = tb.tb_frame
    lineno = tb.tb_lineno
    filename = f.f_code.co_filename
    linecache.checkcache(filename)
    line = linecache.getline(filename, lineno, f.f_globals)
    print ('TEST FAILED IN {}:{} {} \n\t Expected: {} \n\t Returned: {}'.format(filename, lineno, line.strip(),
                                                                                expect_value, return_value))


def check_type(expect_value, return_value):
    if type(expect_value) is NoneType:
        tmp = []
        if (type(return_value) is not type(tmp)) and (type(return_value) is not type('')):
            raise RpcException(expect_value, return_value)
    else:
        if type(expect_value) is not type(return_value):
            raise RpcException(expect_value, return_value)

    return type(expect_value)


def verify_equal(expect_value, return_value):
    type_values = check_type(expect_value, return_value)

    if type_values is IntType:
        if expect_value != return_value:
            raise RpcException(expect_value, return_value)

    elif type_values is ListType:
        if expect_value != return_value:
            raise RpcException(expect_value, return_value)

    elif type_values is BooleanType:
        if expect_value is not return_value:
            raise RpcException(expect_value, return_value)

    elif type_values is StringType:
        if not re.search(expect_value, return_value):
            raise RpcException(expect_value, return_value)

    elif type_values is UnicodeType:
        if expect_value not in return_value:
            raise RpcException(expect_value, return_value)

    elif type_values is NoneType:
        if return_value:
            raise RpcException(expect_value, return_value)

    else:
        raise RpcException(expect_value, return_value)

def verify_get_bdevs_methods_null(rpc_py):
    rpc = spdk_rpc(rpc_py)
    output = rpc.get_bdevs()
    jsonvalue = json.loads(output)
    verify_equal(None, jsonvalue)

    print ("verify_get_bdevs_null_methods passed")


def verify_construct_malloc_bdevs(rpc_py, rpc_param):
    rpc = spdk_rpc(rpc_py)
    output = rpc.construct_malloc_bdev(rpc_param['malloc_bdev_size'], rpc_param['malloc_block_size']).rstrip('\n')
    verify_equal("^Malloc[0-9]+$", output)

    print ("verify_construct_malloc_bdevs_methods passed")
    return output

def verify_get_bdevs_methods(rpc_py, rpc_param):
    rpc = spdk_rpc(rpc_py)
    output = rpc.get_bdevs()
    jsonvalue = json.loads(output)
    verify_equal(rpc_param['malloc_block_size'], jsonvalue[0]['block_size'])

    print ("verify_get_bdevs_methods passed")


def verify_construct_vhost_scsi_controller(rpc_py, rpc_param):
    rpc = spdk_rpc(rpc_py)
    output = rpc.construct_vhost_scsi_controller(rpc_param['ctrlr'])
    verify_equal(None, output)

    print ("verify_construct_vhost_scsi_controller passed")


def verify_get_vhost_scsi_controllers(rpc_py, rpc_param, malloc):
    rpc = spdk_rpc(rpc_py)
    output = rpc.get_vhost_scsi_controllers()
    jsonvalue = json.loads(output)[0]

    verify_equal(rpc_param['ctrlr'], jsonvalue['ctrlr'])

    if jsonvalue['scsi_devs']:
        verify_equal(unicode(malloc), jsonvalue['scsi_devs'][0]['luns'][0]['name'])
    else:
        verify_equal(rpc_param['scsi_devs'], jsonvalue['scsi_devs'])

    verify_equal(rpc_param['cpu_mask'], jsonvalue['cpu_mask'])

    print ("verify_get_vhost_scsi_controllers_methods passed")


def verify_add_vhost_scsi_lun(rpc_py, rpc_param, malloc):
    rpc = spdk_rpc(rpc_py)
    output = rpc.add_vhost_scsi_lun(rpc_param['ctrlr'], rpc_param['scsi_dev_num'], malloc)
    verify_equal(None, output)

    print ("verify_add_vhost_scsi_lun_methods passed")


def verify_remove_vhost_scsi_dev(rpc_py, rpc_param):
    rpc = spdk_rpc(rpc_py)
    output = rpc.remove_vhost_scsi_dev(rpc_param['ctrlr'], rpc_param['scsi_dev_num'])
    verify_equal(None, output)

    print ("verify_remove_vhost_scsi_dev_methods passed")


def verify_remove_vhost_scsi_controller(rpc_py, rpc_param):
    rpc = spdk_rpc(rpc_py)
    output = rpc.remove_vhost_scsi_controller(rpc_param['ctrlr'])
    verify_equal(None, output)

    print ("verify_remove_vhost_scsi_controller_methods passed")


def verify_delete_bdev(rpc_py, malloc):
    rpc = spdk_rpc(rpc_py)
    output = rpc.delete_bdev(malloc)
    verify_equal(None, output)

    print ("verify_delete_bdev_methods passed")

if __name__ == "__main__":

    rpc_py = sys.argv[1]

    try:
        verify_get_bdevs_methods_null(rpc_py)
        malloc = verify_construct_malloc_bdevs(rpc_py, rpc_param)
        verify_get_bdevs_methods(rpc_py, rpc_param)
        verify_construct_vhost_scsi_controller(rpc_py, rpc_param)
        verify_get_vhost_scsi_controllers(rpc_py, rpc_param, malloc)
        verify_get_bdevs_methods(rpc_py, rpc_param)
        verify_add_vhost_scsi_lun(rpc_py, rpc_param, malloc)
        verify_get_vhost_scsi_controllers(rpc_py, rpc_param_second, malloc)
        verify_remove_vhost_scsi_dev(rpc_py, rpc_param)
        verify_get_vhost_scsi_controllers(rpc_py, rpc_param, malloc)
        verify_remove_vhost_scsi_controller(rpc_py, rpc_param)
        verify_get_bdevs_methods(rpc_py, rpc_param)
        verify_delete_bdev(rpc_py, malloc)
        verify_get_bdevs_methods_null(rpc_py)

    except RpcException as e:
        print_error(e.exprval, e.retval)
