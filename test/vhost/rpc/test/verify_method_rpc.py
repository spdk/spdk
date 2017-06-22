#!/usr/bin/env python

import json
import re

from types import *
from subprocess import check_output


class RpcException(Exception):

    def __init__(self, expect_value, return_value, *args):
        super(RpcException, self).__init__(*args)
        self.retval = return_value
        self.exprval = expect_value

class Spdk_rpc(object):
    def __init__(self, rpc_py):
        self.rpc_py = rpc_py

    def __getattr__(self, name):
        def call(*args):
            cmd = "python {} {}".format(self.rpc_py, name)
            for arg in args:
                cmd += " {}".format(arg)
            return check_output(cmd, shell=True)
        return call

class Verify_rpc_method(globals()):
    def __init__(self, rpc_py):
        self.rpc = self.Spdk_rpc(rpc_py)


    def check_type(self, expect_value, return_value):
        """
        The method checks the value type.
        :param expect_value: Expected execution value of the rpc method.
        :param return_value: Returned execution value of the rpc method.
        :return: Type of the expected values or optional expect_value,
        return_value to exception.
        """

        if type(expect_value) is NoneType:
            tmp = []
            if (type(return_value) is not type(tmp)) and \
                    (type(return_value) is not type('')):
                raise RpcException(expect_value, return_value)
        else:
            if type(expect_value) is not type(return_value):
                raise RpcException(expect_value, return_value)

        return type(expect_value)

    def verify_equal(self, expect_value, return_value):
        """
        The method checks whether of the expected value is equal return value.
        :param expect_value: Expected execution value of the rpc method.
        :param return_value: Returned execution value of the rpc method.
        :return: Optional expect_value, return_value to exception.
        """

        type_values = self.check_type(self, expect_value, return_value)

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

    def verify_get_bdevs_methods_null(self):

        output = self.rpc.get_bdevs()
        jsonvalue = json.loads(output)
        self.verify_equal(None, jsonvalue)

        print("verify_get_bdevs_null_methods passed")


    def verify_construct_malloc_bdevs(self, rpc_param):

        output = self.rpc.construct_malloc_bdev(
                rpc_param['malloc_bdev_size'],
                rpc_param['malloc_block_size']).rstrip('\n')

        self.verify_equal("^Malloc[0-9]+$", output)

        print("verify_construct_malloc_bdevs_methods passed")
        return output


    def verify_get_bdevs_methods(self, rpc_param):

        output = self.rpc.get_bdevs()
        jsonvalue = json.loads(output)
        self.verify_equal(rpc_param['malloc_block_size'], jsonvalue[0]['block_size'])

        print("verify_get_bdevs_methods passed")


    def verify_construct_vhost_scsi_controller(self, rpc_param):

        output = self.rpc.construct_vhost_scsi_controller(rpc_param['ctrlr'])
        self.verify_equal(None, output)

        print("verify_construct_vhost_scsi_controller passed")


    def verify_get_vhost_scsi_controllers(self, rpc_param, malloc):

        output = self.rpc.get_vhost_scsi_controllers()
        jsonvalue = json.loads(output)[0]

        self.verify_equal(rpc_param['ctrlr'], jsonvalue['ctrlr'])

        if jsonvalue['scsi_devs']:
            self.verify_equal(unicode(malloc),
                         jsonvalue['scsi_devs'][0]['luns'][0]['name'])
        else:
            self.verify_equal(rpc_param['scsi_devs'], jsonvalue['scsi_devs'])

        self.verify_equal(rpc_param['cpu_mask'], jsonvalue['cpu_mask'])

        print("verify_get_vhost_scsi_controllers_methods passed")


    def verify_add_vhost_scsi_lun(self, rpc_param, malloc):

        output = self.rpc.add_vhost_scsi_lun(rpc_param['ctrlr'],
                                        rpc_param['scsi_dev_num'], malloc)
        self.verify_equal(None, output)

        print("verify_add_vhost_scsi_lun_methods passed")


    def verify_remove_vhost_scsi_dev(self, rpc_param):

        output = self.rpc.remove_vhost_scsi_dev(rpc_param['ctrlr'],
                                           rpc_param['scsi_dev_num'])
        self.verify_equal(None, output)

        print("verify_remove_vhost_scsi_dev_methods passed")


    def verify_remove_vhost_scsi_controller(self, rpc_param):

        output = self.rpc.remove_vhost_scsi_controller(rpc_param['ctrlr'])
        self.verify_equal(None, output)

        print("verify_remove_vhost_scsi_controller_methods passed")


    def verify_delete_bdev(self,  malloc):

        output = self.rpc.delete_bdev(malloc)
        self.verify_equal(None, output)

        print("verify_delete_bdev_methods passed")
