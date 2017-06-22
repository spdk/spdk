#!/usr/bin/env python

import sys
from verify_method_rpc import *

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



if __name__ == "__main__":

    rpc_py = sys.argv[1]

    try:
        Verify_rpc_method.verify_get_bdevs_methods_null(rpc_py)
        malloc = Verify_rpc_method.verify_construct_malloc_bdevs(rpc_py,
                                                                 rpc_param)
        Verify_rpc_method.verify_get_bdevs_methods(rpc_py, rpc_param)
        Verify_rpc_method.verify_construct_vhost_scsi_controller(rpc_py,
                                                                 rpc_param)
        Verify_rpc_method.verify_get_vhost_scsi_controllers(rpc_py,
                                                            rpc_param, malloc)
        Verify_rpc_method.verify_get_bdevs_methods(rpc_py, rpc_param)
        Verify_rpc_method.verify_add_vhost_scsi_lun(rpc_py, rpc_param, malloc)
        Verify_rpc_method.verify_get_vhost_scsi_controllers(rpc_py,
                                                            rpc_param_second,
                                                            malloc)
        Verify_rpc_method.verify_remove_vhost_scsi_dev(rpc_py, rpc_param)
        Verify_rpc_method.verify_get_vhost_scsi_controllers(rpc_py,
                                                            rpc_param, malloc)
        Verify_rpc_method.verify_remove_vhost_scsi_controller(rpc_py,
                                                              rpc_param)
        Verify_rpc_method.verify_get_bdevs_methods(rpc_py, rpc_param)
        Verify_rpc_method.verify_delete_bdev(rpc_py, malloc)
        Verify_rpc_method.verify_get_bdevs_methods_null(rpc_py)

    except RpcException as e:
        Verify_rpc_method.print_error(e.exprval, e.retval)
