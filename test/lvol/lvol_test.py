#!/usr/bin/env python
import sys
from Commands_Rpc_Lib import Commands_Rpc

class Test_Cases(object):
    def __init__ (self, rpc_py):
        self.c = Commands_Rpc(rpc_py)
### positive tests
    def test_case1(self, total_size, block_size):
        print("===========Test Case 1: Start===========")
        print("==========construct_lvs_positive========")
        base_name = self.c.construct_malloc_bdev(total_size,block_size)
        print("PASSED: RPC COMMAND: construct_malloc_bdev ")
        uuid_store = self.c.construct_lvol_store(base_name)
        self.c.get_lvol_stores (base_name, uuid_store)
        print("PASSED: RPC COMMAND: construct_lvol_store")
        self.c.delete_bdev(base_name)
        print("PASSED: RPC COMMAND: delete_bdev")
        print("===========Test Case 1: END===========\n")
        
    def test_case2(self, total_size, block_size):
        print("===========Test Case 2: Start===========")
        print("===construct_logical_volume_positive====")
        base_name = self.c.construct_malloc_bdev (total_size, block_size)
        print("PASSED: RPC COMMAND: construct_malloc_bdev ")
        uuid_store = self.c.construct_lvol_store(base_name)
        self.c.get_lvol_stores (base_name, uuid_store)
        print("PASSED: RPC COMMAND: construct_lvol_store")
        uuid_bdev = self.c.construct_lvol_bdev(uuid_store, total_size-1)
        self.c.check_get_bdevs_methods(uuid_bdev)
        print("PASSED: RPC COMMAND: construct_lvol_bdev")
        self.c.delete_bdev(base_name)
        print("PASSED: RPC COMMAND: delete_bdev")
        print("===========Test Case 2: END===========\n")
    
    def test_case3(self, total_size, block_size):
        print("===========Test Case 3: Start===========")
        print("construct_multi_logical_volumes_positive")
        base_name = self.c.construct_malloc_bdev (total_size, block_size)
        print("PASSED: RPC COMMAND: construct_malloc_bdev ")
        uuid_store = self.c.construct_lvol_store(base_name)
        self.c.get_lvol_stores (base_name, uuid_store)
        print("PASSED: RPC COMMAND: construct_lvol_store")
        size = ((total_size-1)/4)
        for _ in range(4):
            uuid_bdev = self.c.construct_lvol_bdev(uuid_store, size)
            self.c.check_get_bdevs_methods(uuid_bdev)
        print("PASSED: RPC COMMAND: construct_lvol_bdev")
        self.c.delete_bdev (base_name)
        print("PASSED: RPC COMMAND: delete_bdev")
        print("===========Test Case 3: END===========\n")
    
    def test_case4(self, total_size, block_size):
        print("===========Test Case 4: Start===========")
        print("===construct_logical_volume_positive====")
        base_name = self.c.construct_malloc_bdev (total_size, block_size)
        print("PASSED: RPC COMMAND: construct_malloc_bdev ")
        uuid_store = self.c.construct_lvol_store(base_name)
        self.c.get_lvol_stores (base_name, uuid_store)
        print("PASSED: RPC COMMAND: construct_lvol_store")
        uuid_bdev = self.c.construct_lvol_bdev(uuid_store, total_size-1)
        self.c.check_get_bdevs_methods(uuid_bdev)
        print("PASSED: RPC COMMAND: construct_lvol_bdev")
        self.c.resize_lvol_bdev(uuid_bdev, total_size/2)
        print("PASSED: RPC COMMAND: resize_lvol_bdev - one quarter size of bdev")
        self.c.resize_lvol_bdev (uuid_bdev, total_size/4)
        print("PASSED: RPC COMMAND: resize_lvol_bdev - half size of bdev")
        self.c.resize_lvol_bdev (uuid_bdev, total_size-1)
        print("PASSED: RPC COMMAND: resize_lvol_bdev - full size of bdev")
        self.c.delete_bdev (base_name)
        print("PASSED: RPC COMMAND: delete_bdev")
        print("===========Test Case 4: END===========\n")


    def test_case5(self, total_size, block_size):
        print("===========Test Case 5: Start===========")
        print("=====destroy_lvol_store_positive========")
        base_name = self.c.construct_malloc_bdev (total_size, block_size)
        print("PASSED: RPC COMMAND: construct_malloc_bdev ")
        uuid_store = self.c.construct_lvol_store (base_name)
        self.c.get_lvol_stores (base_name, uuid_store)
        print("PASSED: RPC COMMAND: construct_lvol_store")
        self.c.destroy_lvol_store(uuid_store)
        print("PASSED: RPC COMMAND: destroy_lvol_store")
        self.c.get_lvol_stores (None, None)
        print("PASSED: RPC COMMAND: construct_lvol_store")
        self.c.delete_bdev (base_name)
        print("PASSED: RPC COMMAND: delete_bdev")
        print("===========Test Case 5: END===========\n")


    def test_case6(self, total_size, block_size):
        print("===========Test Case 6: Start===========")
        print("===construct_logical_volume_positive====")
        base_name = self.c.construct_malloc_bdev (total_size, block_size)
        print("PASSED: RPC COMMAND: construct_malloc_bdev ")
        uuid_store = self.c.construct_lvol_store (base_name)
        self.c.get_lvol_stores (base_name, uuid_store)
        print("PASSED: RPC COMMAND: construct_lvol_store")
        uuid_bdev = self.c.construct_lvol_bdev (uuid_store, total_size-1)
        self.c.check_get_bdevs_methods (uuid_bdev)
        print("PASSED: RPC COMMAND: construct_lvol_bdev")
        self.c.destroy_lvol_store (uuid_store)
        print("PASSED: RPC COMMAND: destroy_lvol_store")
        self.c.get_lvol_stores (None, None)
        print("PASSED: RPC COMMAND: construct_lvol_store")
        self.c.delete_bdev (base_name)
        print("PASSED: RPC COMMAND: delete_bdev")
        print("===========Test Case 6: END===========\n")
    
    
    def test_case7 (self, total_size, block_size):
        print("===========Test Case 7: Start===========")
        print("construct_multi_logical_volumes_positive")
        base_name = self.c.construct_malloc_bdev (total_size, block_size)
        print("PASSED: RPC COMMAND: construct_malloc_bdev ")
        uuid_store = self.c.construct_lvol_store (base_name)
        self.c.get_lvol_stores (base_name, uuid_store)
        print("PASSED: RPC COMMAND: construct_lvol_store")
        size = ((total_size - 1) / 4)
        for _ in range (4):
            uuid_bdev = self.c.construct_lvol_bdev (uuid_store, size)
            self.c.check_get_bdevs_methods (uuid_bdev)
        print("PASSED: RPC COMMAND: construct_lvol_bdev")
        self.c.destroy_lvol_store (uuid_store)
        print("PASSED: RPC COMMAND: destroy_lvol_store")
        self.c.get_lvol_stores (None, None)
        print("PASSED: RPC COMMAND: construct_lvol_store")
        self.c.delete_bdev (base_name)
        print("PASSED: RPC COMMAND: delete_bdev")
        print("===========Test Case 7: END===========\n")
        
    def test_case8(self, total_size, block_size):
        print("===========Test Case 8: Start===========")
        print("nasted construct_logical_volume_positive")
        base_name = self.c.construct_malloc_bdev (total_size, block_size)
        print("PASSED: RPC COMMAND: construct_malloc_bdev ")
        uuid_store = self.c.construct_lvol_store(base_name)
        self.c.get_lvol_stores (base_name, uuid_store)
        print("PASSED: RPC COMMAND: construct_lvol_store")
        uuid_bdev = self.c.construct_lvol_bdev(uuid_store, total_size-1)
        self.c.check_get_bdevs_methods(uuid_bdev)
        print("PASSED: RPC COMMAND: construct_lvol_bdev")
        uuid_store2 = self.c.construct_lvol_store(uuid_bdev)
        self.c.get_lvol_stores (uuid_bdev, uuid_store2)
        print("PASSED: RPC COMMAND: construct_lvol_store")
        self.c.delete_bdev(base_name)
        print("PASSED: RPC COMMAND: delete_bdev")
        print("===========Test Case 8: END===========\n")

if __name__ == "__main__":
    total_size = 64
    block_size = 512
    
    rpc_py = sys.argv [1]
    
    if len(sys.argv) == 4:
        total_size = sys.argv [2]
        block_size = sys.argv [3]
        
    tc = Test_Cases(rpc_py)
    ### positive tests
    # tc.test_case1(total_size,block_size)
    # tc.test_case2(total_size,block_size)
    # tc.test_case3(total_size,block_size)
    # tc.test_case4(total_size,block_size)
    tc.test_case5(total_size,block_size)
    tc.test_case6(total_size,block_size)
    tc.test_case7(total_size,block_size)
    tc.test_case8(total_size,block_size)
    
    
    
	

