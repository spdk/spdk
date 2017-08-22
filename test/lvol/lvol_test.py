#!/usr/bin/env python
import sys
from Commands_Rpc_Lib import Commands_Rpc

def test_case1(rpc_py, total_size, block_size):
    c = Commands_Rpc(rpc_py)
    print("===========Test Case 1: Start===========")
    print("==========construct_lvs_positive========")
    base_name = c.construct_malloc_bdev(total_size,block_size)
    print("PASSED: RPC COMMAND: construct_malloc_bdev ")
    uuid_store = c.construct_lvol_store(base_name)
    c.get_lvol_stores (base_name, uuid_store)
    print("PASSED: RPC COMMAND: construct_lvol_store")
    c.delete_bdev(base_name)
    print("PASSED: RPC COMMAND: delete_bdev")
    print("===========Test Case 1: END===========\n")
    
def test_case2(rpc_py, total_size, block_size):
    c = Commands_Rpc (rpc_py)
    print("===========Test Case 2: Start===========")
    print("===construct_logical_volume_positive====")
    base_name = c.construct_malloc_bdev (total_size, block_size)
    print("PASSED: RPC COMMAND: construct_malloc_bdev ")
    uuid_store = c.construct_lvol_store(base_name)
    c.get_lvol_stores (base_name, uuid_store)
    print("PASSED: RPC COMMAND: construct_lvol_store")
    uuid_bdev = c.construct_lvol_bdev(uuid_store, 32)
    c.check_get_bdevs_methods(uuid_bdev)
    print("PASSED: RPC COMMAND: construct_lvol_bdev")
    c.delete_bdev(base_name)
    print("PASSED: RPC COMMAND: delete_bdev")
    print("===========Test Case 2: END===========\n")

def test_case3(rpc_py, total_size, block_size):
    c = Commands_Rpc (rpc_py)
    print("===========Test Case 3: Start===========")
    print("construct_multi_logical_volumes_positive")
    base_name = c.construct_malloc_bdev (total_size, block_size)
    print("PASSED: RPC COMMAND: construct_malloc_bdev ")
    uuid_store = c.construct_lvol_store(base_name)
    c.get_lvol_stores (base_name, uuid_store)
    print("PASSED: RPC COMMAND: construct_lvol_store")
    size = ((total_size-1)/4)
    for _ in range(4):
        uuid_bdev = c.construct_lvol_bdev(uuid_store, size)
        c.check_get_bdevs_methods(uuid_bdev)
    print("PASSED: RPC COMMAND: construct_lvol_bdev")
    c.delete_bdev (base_name)
    print("PASSED: RPC COMMAND: delete_bdev")
    print("===========Test Case 3: END===========\n")

def test_case4(rpc_py, total_size, block_size):
    c = Commands_Rpc (rpc_py)
    print("===========Test Case 4: Start===========")
    print("===construct_logical_volume_positive====")
    base_name = c.construct_malloc_bdev (total_size, block_size)
    print("PASSED: RPC COMMAND: construct_malloc_bdev ")
    uuid_store = c.construct_lvol_store(base_name)
    c.get_lvol_stores (base_name, uuid_store)
    print("PASSED: RPC COMMAND: construct_lvol_store")
    uuid_bdev = c.construct_lvol_bdev(uuid_store, 32)
    c.check_get_bdevs_methods(uuid_bdev)
    print("PASSED: RPC COMMAND: construct_lvol_bdev")
    c.resize_lvol_bdev(uuid_bdev, 16)
    print("PASSED: RPC COMMAND: resize_lvol_bdev - one quarter size of bdev")
    c.resize_lvol_bdev (uuid_bdev, 32)
    print("PASSED: RPC COMMAND: resize_lvol_bdev - half size of bdev")
    c.resize_lvol_bdev (uuid_bdev, 63)
    print("PASSED: RPC COMMAND: resize_lvol_bdev - full size of bdev")
    c.delete_bdev (base_name)
    print("PASSED: RPC COMMAND: delete_bdev")
    print("===========Test Case 4: END===========\n")


def test_case5(rpc_py, total_size, block_size):
    c = Commands_Rpc (rpc_py)
    print("===========Test Case 5: Start===========")
    print("=====destroy_lvol_store_positive========")
    base_name = c.construct_malloc_bdev (total_size, block_size)
    print("PASSED: RPC COMMAND: construct_malloc_bdev ")
    uuid_store = c.construct_lvol_store (base_name)
    c.get_lvol_stores (base_name, uuid_store)
    print("PASSED: RPC COMMAND: construct_lvol_store")
    c.destroy_lvol_store(uuid_store)
    print("PASSED: RPC COMMAND: destroy_lvol_store")
    c.get_lvol_stores (None, None)
    print("PASSED: RPC COMMAND: construct_lvol_store")
    c.delete_bdev (base_name)
    print("PASSED: RPC COMMAND: delete_bdev")
    print("===========Test Case 5: END===========\n")


def test_case6(rpc_py, total_size, block_size):
    c = Commands_Rpc (rpc_py)
    print("===========Test Case 6: Start===========")
    print("===construct_logical_volume_positive====")
    base_name = c.construct_malloc_bdev (total_size, block_size)
    print("PASSED: RPC COMMAND: construct_malloc_bdev ")
    uuid_store = c.construct_lvol_store (base_name)
    c.get_lvol_stores (base_name, uuid_store)
    print("PASSED: RPC COMMAND: construct_lvol_store")
    uuid_bdev = c.construct_lvol_bdev (uuid_store, 32)
    c.check_get_bdevs_methods (uuid_bdev)
    print("PASSED: RPC COMMAND: construct_lvol_bdev")
    c.destroy_lvol_store (uuid_store)
    print("PASSED: RPC COMMAND: destroy_lvol_store")
    c.get_lvol_stores (None, None)
    print("PASSED: RPC COMMAND: construct_lvol_store")
    c.delete_bdev (base_name)
    print("PASSED: RPC COMMAND: delete_bdev")
    print("===========Test Case 6: END===========\n")


def test_case7 (rpc_py, total_size, block_size):
    c = Commands_Rpc (rpc_py)
    print("===========Test Case 7: Start===========")
    print("construct_multi_logical_volumes_positive")
    base_name = c.construct_malloc_bdev (total_size, block_size)
    print("PASSED: RPC COMMAND: construct_malloc_bdev ")
    uuid_store = c.construct_lvol_store (base_name)
    c.get_lvol_stores (base_name, uuid_store)
    print("PASSED: RPC COMMAND: construct_lvol_store")
    size = ((total_size - 1) / 4)
    for _ in range (4):
        uuid_bdev = c.construct_lvol_bdev (uuid_store, size)
        c.check_get_bdevs_methods (uuid_bdev)
    print("PASSED: RPC COMMAND: construct_lvol_bdev")
    c.destroy_lvol_store (uuid_store)
    print("PASSED: RPC COMMAND: destroy_lvol_store")
    c.get_lvol_stores (None, None)
    print("PASSED: RPC COMMAND: construct_lvol_store")
    c.delete_bdev (base_name)
    print("PASSED: RPC COMMAND: delete_bdev")
    print("===========Test Case 7: END===========\n")
    
def test_case8(rpc_py, total_size, block_size):
    c = Commands_Rpc (rpc_py)
    print("===========Test Case 8: Start===========")
    print("nasted construct_logical_volume_positive")
    base_name = c.construct_malloc_bdev (total_size, block_size)
    print("PASSED: RPC COMMAND: construct_malloc_bdev ")
    uuid_store = c.construct_lvol_store(base_name)
    c.get_lvol_stores (base_name, uuid_store)
    print("PASSED: RPC COMMAND: construct_lvol_store")
    uuid_bdev = c.construct_lvol_bdev(uuid_store, total_size-1)
    c.check_get_bdevs_methods(uuid_bdev)
    print("PASSED: RPC COMMAND: construct_lvol_bdev")
    uuid_store2 = c.construct_lvol_store(uuid_bdev)
    c.get_lvol_stores (uuid_bdev, uuid_store2)
    print("PASSED: RPC COMMAND: construct_lvol_store")
    c.delete_bdev(base_name)
    print("PASSED: RPC COMMAND: delete_bdev")
    print("===========Test Case 8: END===========\n")

if __name__ == "__main__":
    total_size = 64
    block_size = 512
    
    rpc_py = sys.argv [1]
    
    if len(sys.argv) == 4:
        total_size = sys.argv [2]
        block_size = sys.argv [3]
   
    ###positive tests
    test_case1(rpc_py,total_size,block_size)
    test_case2(rpc_py,total_size,block_size)
    test_case3(rpc_py,total_size,block_size)
    test_case4(rpc_py,total_size,block_size)
    test_case5(rpc_py,total_size,block_size)
    test_case6(rpc_py,total_size,block_size)
    test_case7(rpc_py,total_size,block_size)
    test_case8(rpc_py,total_size,block_size)
    
    
    
	

