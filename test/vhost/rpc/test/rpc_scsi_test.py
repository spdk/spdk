#!/usr/bin/env python

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


def test_basic(rpc_py):
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

def test_2(rpc_py):
    try:
        verify_get_bdevs_methods_null(rpc_py)
        malloc = verify_construct_malloc_bdevs(rpc_py, rpc_param)
        verify_get_bdevs_methods(rpc_py, rpc_param)
        verify_delete_bdev(rpc_py, malloc)
        verify_get_bdevs_methods_null(rpc_py)
    except RpcException as e:
        print_error(e.exprval, e.retval)

def test_3(rpc_py):
    try:
        verify_get_bdevs_methods(rpc_py, rpc_param)
        verify_construct_vhost_scsi_controller(rpc_py, rpc_param)
        verify_remove_vhost_scsi_controller(rpc_py, rpc_param)
    except RpcException as e:
        print_error(e.exprval, e.retval)

def test_4(rpc_py, malloc):
    try:
        verify_get_bdevs_methods(rpc_py, rpc_param)
        verify_construct_vhost_scsi_controller(rpc_py, rpc_param)
        verify_get_vhost_scsi_controllers(rpc_py, rpc_param, malloc)
        verify_add_vhost_scsi_lun(rpc_py, rpc_param, malloc)
        verify_remove_vhost_scsi_dev(rpc_py, rpc_param)
        verify_remove_vhost_scsi_controller(rpc_py, rpc_param)
    except RpcException as e:
        print_error(e.exprval, e.retval)

def test_5(rpc_py, malloc):
    try:
        verify_get_bdevs_methods(rpc_py, rpc_param)
        verify_construct_vhost_scsi_controller(rpc_py, rpc_param)
        verify_get_vhost_scsi_controllers(rpc_py, rpc_param, malloc)
        verify_add_vhost_scsi_lun(rpc_py, rpc_param, malloc)
        verify_remove_vhost_scsi_controller(rpc_py, rpc_param)
    except RpcException as e:
        print_error(e.exprval, e.retval)

if __name__ == "__main__":
    # sys.argv[1] - test name
    # sys.argv[2] - rpc path
    # sys.argv[3] - bdevs name

    if sys.argv[1] in ["test_base"]:
        test_basic(sys.argv[1])
    elif sys.argv[1] in ["test2"]:
        test_2(sys.argv[2])
    elif sys.argv[1] in ["test3"]:
        test_3(sys.argv[2])
    elif sys.argv[1] in ["test4"]:
        test_4(sys.argv[2], sys.argv[3])
    elif sys.argv[1] in ["test5"]:
        test_5(sys.argv[2], sys.argv[3])
    else:
        "Error: Bad choose test"