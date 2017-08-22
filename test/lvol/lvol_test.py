#!/usr/bin/env python
import sys
from os import *

from Commands_Rpc_Lib import Commands_Rpc


def header(num):
    test_name = {
        1: 'construct_lvs_positive',
        2: 'construct_logical_volume_positiv',
        3: 'construct_multi_logical_volumes_positive',
        4: 'construct_logical_volume_positive',
        5: 'destroy_lvol_store_positive',
        6: 'construct_logical_volume_positive',
        7: 'construct_multi_logical_volumes_positive',
        8: 'nasted construct_logical_volume_positive',
        9: 'construct_lvs_positive',
        10: 'construct_lvs_on_bdev_twic_negative',
        11: 'construct_logical_volume_nonexistent_lvs_uuid',
        12: 'construct_logical_volumes_on_busy_bdev',
        13: 'resize_logical_volume_nonexistent_logical_volume',
        14: 'resize_logical_volume_with_size_out_of_range',
        15: 'destroy_lvol_store_nonexistent_lvs_uuid',
        16: 'destroy_lvol_store_nonexistent_bdev',
        17: 'nasted construct_logical_volume_nonexistent',
        18: 'nasted construct_logical_volume_negative',
    }
    print("========================================================")
    print("Test Case{num}: Start".format(num=num))
    print("Test Name: {name}".format(name=test_name[num]))
    print("========================================================")
    
def footer(num):
    print("RESULT: PASSED".format(num=num))
    print("Test Case{num}: END\n".format(num=num))
    print("========================================================")
    
class Test_Cases(object):
    def __init__(self, rpc_py):
        self.c = Commands_Rpc(rpc_py)
    ### positive tests
    def test_case1(self, total_size, block_size):
        header(1)
        base_name = self.c.construct_malloc_bdev(total_size,block_size)
        uuid_store = self.c.construct_lvol_store(base_name)
        fail_count = self.c.get_lvol_stores(base_name, uuid_store)
        self.c.destroy_lvol_store(uuid_store)
        self.c.delete_bdev(base_name)
        footer(1)
        return fail_count
    def test_case2(self, total_size, block_size):
        header(2)
        base_name = self.c.construct_malloc_bdev(total_size, block_size)
        uuid_store = self.c.construct_lvol_store(base_name)
        fail_count = self.c.get_lvol_stores(base_name, uuid_store)
        uuid_bdev = self.c.construct_lvol_bdev(uuid_store, total_size-1)
        fail_count += self.c.check_get_bdevs_methods(uuid_bdev)
        self.c.destroy_lvol_store(uuid_store)
        self.c.delete_bdev(base_name)
        footer(2)
        return fail_count
    def test_case3(self, total_size, block_size):
        header(3)
        base_name = self.c.construct_malloc_bdev(total_size, block_size)
        uuid_store = self.c.construct_lvol_store(base_name)
        fail_count = self.c.get_lvol_stores(base_name, uuid_store)
        size =((total_size-1)/4)
        for _ in range(4):
            uuid_bdev = self.c.construct_lvol_bdev(uuid_store, size)
            fail_count += self.c.check_get_bdevs_methods(uuid_bdev)
        self.c.destroy_lvol_store(uuid_store)
        self.c.delete_bdev(base_name)
        footer(3)
        return fail_count
    def test_case4(self, total_size, block_size):
        header(4)
        base_name = self.c.construct_malloc_bdev(total_size, block_size)
        uuid_store = self.c.construct_lvol_store(base_name)
        fail_count = self.c.get_lvol_stores(base_name, uuid_store)
        uuid_bdev = self.c.construct_lvol_bdev(uuid_store, total_size-1)
        fail_count += self.c.check_get_bdevs_methods(uuid_bdev)
        self.c.resize_lvol_bdev(uuid_bdev, total_size/2)
        self.c.resize_lvol_bdev(uuid_bdev, total_size/4)
        self.c.resize_lvol_bdev(uuid_bdev, total_size-1)
        self.c.destroy_lvol_store(uuid_store)
        self.c.delete_bdev(base_name)
        footer(4)
        return fail_count
    def test_case5(self, total_size, block_size):
        header(5)
        base_name = self.c.construct_malloc_bdev(total_size, block_size)
        uuid_store = self.c.construct_lvol_store(base_name)
        fail_count = self.c.get_lvol_stores(base_name, uuid_store)
        self.c.destroy_lvol_store(uuid_store)
        fail_count += self.c.get_lvol_stores(None, None)
        self.c.delete_bdev(base_name)
        footer(5)
        return fail_count
    def test_case6(self, total_size, block_size):
        header(6)
        base_name = self.c.construct_malloc_bdev(total_size, block_size)
        uuid_store = self.c.construct_lvol_store(base_name)
        fail_count = self.c.get_lvol_stores(base_name, uuid_store)
        uuid_bdev = self.c.construct_lvol_bdev(uuid_store, total_size-1)
        fail_count += self.c.check_get_bdevs_methods(uuid_bdev)
        self.c.destroy_lvol_store(uuid_store)
        fail_count += self.c.get_lvol_stores(None, None)
        self.c.delete_bdev(base_name)
        footer(6)
        return fail_count
    def test_case7(self, total_size, block_size):
        header(7)
        base_name = self.c.construct_malloc_bdev(total_size, block_size)
        uuid_store = self.c.construct_lvol_store(base_name)
        fail_count = self.c.get_lvol_stores(base_name, uuid_store)
        size =((total_size - 1) / 4)
        for _ in range(4):
            uuid_bdev = self.c.construct_lvol_bdev(uuid_store, size)
            fail_count += self.c.check_get_bdevs_methods(uuid_bdev)
        self.c.destroy_lvol_store(uuid_store)
        fail_count += self.c.get_lvol_stores(None, None)
        self.c.delete_bdev(base_name)
        footer(7)
        return fail_count
    def test_case8(self, total_size, block_size):
        header(8)
        base_name = self.c.construct_malloc_bdev(total_size, block_size)
        uuid_store = self.c.construct_lvol_store(base_name)
        fail_count = self.c.get_lvol_stores(base_name, uuid_store)
        uuid_bdev = self.c.construct_lvol_bdev(uuid_store, total_size-1)
        fail_count += self.c.check_get_bdevs_methods(uuid_bdev)
        uuid_store2 = self.c.construct_lvol_store(uuid_bdev)
        fail_count += self.c.get_lvol_stores(uuid_bdev, uuid_store2)
        self.c.delete_bdev(base_name)
        footer(8)
        return fail_count
    ### negative tests
    def test_case9(self):
        header(9)
        try:
            self.c.construct_lvol_store("123365454")
            print("FAILED: RPC COMMAND: construct_lvol_store")
            return 1
        except:
            pass
        footer(9)
        return 0
    def test_case10(self, total_size, block_size):
        header(10)
        base_name = self.c.construct_malloc_bdev(total_size, block_size)
        uuid_store = self.c.construct_lvol_store(base_name)
        fail_count = self.c.get_lvol_stores(base_name, uuid_store)
        try:
            self.c.construct_lvol_store(base_name)
            print("FAILED: RPC COMMAND: construct_lvol_store")
            return 1
        except:
            pass
        self.c.destroy_lvol_store(uuid_store)
        self.c.delete_bdev(base_name)
        footer(10)
        return fail_count
    def test_case11(self):
        header(11)
        try:
            self.c.construct_lvol_bdev("1234", 32)
            print("PASSED: RPC COMMAND: construct_lvol_bdev")
            return 1
        except:
            pass
        footer(11)
        return 0
    def test_case12(self, total_size, block_size):
        header(12)
        base_name = self.c.construct_malloc_bdev(total_size, block_size)
        uuid_store = self.c.construct_lvol_store(base_name)
        fail_count = self.c.get_lvol_stores(base_name, uuid_store)
        uuid_bdev = self.c.construct_lvol_bdev(uuid_store, total_size - 1)
        fail_count += self.c.check_get_bdevs_methods(uuid_bdev)
        try:
            self.c.construct_lvol_bdev(uuid_store, total_size - 1)
            print("FAILED: RPC COMMAND: construct_lvol_bdev")
            return 1
        except:
            pass
        self.c.destroy_lvol_store(uuid_store)
        self.c.delete_bdev(base_name)
        footer(12)
        return fail_count
    def test_case13(self):
        header(13)
        try:
            self.c.resize_lvol_bdev("1234", 16)
            print("FAILED: RPC COMMAND: resize_lvol_bdev")
            return 1
        except:
            pass
        footer(13)
        return 0
    def test_case14(self, total_size, block_size):
        header(14)
        base_name = self.c.construct_malloc_bdev(total_size, block_size)
        uuid_store = self.c.construct_lvol_store(base_name)
        fail_count = self.c.get_lvol_stores(base_name, uuid_store)
        uuid_bdev = self.c.construct_lvol_bdev(uuid_store, total_size-1)
        fail_count += self.c.check_get_bdevs_methods(uuid_bdev)
        try:
            self.c.resize_lvol_bdev(uuid_bdev, total_size+1)
            print("FAILED: RPC COMMAND: resize_lvol_bdev - full size of bdev")
            return 1
        except:
            pass
        self.c.destroy_lvol_store(uuid_store)
        self.c.delete_bdev(base_name)
        footer(14)
        return fail_count
    def test_case15(self):
        header(15)
        try:
            self.c.destroy_lvol_store("1234")
            print("FAILED: RPC COMMAND: destroy_lvol_store")
            exit()
        except:
            pass
        footer(15)
        return 0
    def test_case16(self, total_size, block_size):
        header(16)
        base_name = self.c.construct_malloc_bdev(total_size, block_size)
        uuid_store = self.c.construct_lvol_store(base_name)
        fail_count = self.c.get_lvol_stores(base_name, uuid_store)
        self.c.delete_bdev(base_name)
        try:
            self.c.destroy_lvol_store(uuid_store)
            print("FAILED: RPC COMMAND: destroy_lvol_store")
            return 1
        except:
            pass
        footer(16)
        return fail_count
    def test_case17(self, total_size, block_size):
        header(17)
        base_name = self.c.construct_malloc_bdev(total_size, block_size)
        uuid_store = self.c.construct_lvol_store(base_name)
        fail_count = self.c.get_lvol_stores(base_name, uuid_store)
        uuid_bdev = self.c.construct_lvol_bdev(uuid_store, total_size-1)
        fail_count += self.c.check_get_bdevs_methods(uuid_bdev)
        try:
            uuid_store2 = self.c.construct_lvol_store(base_name)
            print("FAILED: RPC COMMAND: construct_lvol_store "
                  "{uuid}\n".format(uuid=uuid_store2))
            return 1
        except:
            pass
        self.c.delete_bdev(base_name)
        footer(17)
        return fail_count
    def test_case18(self, total_size, block_size):
        header(18)
        base_name = self.c.construct_malloc_bdev(total_size, block_size)
        uuid_store = self.c.construct_lvol_store(base_name)
        fail_count = self.c.get_lvol_stores(base_name, uuid_store)
        uuid_bdev = self.c.construct_lvol_bdev(uuid_store, total_size-1)
        fail_count += self.c.check_get_bdevs_methods(uuid_bdev)
        uuid_store2 = self.c.construct_lvol_store(uuid_bdev)
        fail_count += self.c.get_lvol_stores(uuid_bdev, uuid_store2)
        try:
            self.c.destroy_lvol_store(uuid_store)
            print("FAILED: RPC COMMAND: destroy_lvol_store\n")
            return 1
        except:
            pass
        self.c.delete_bdev(base_name)
        footer(18)
        return fail_count

if __name__ == "__main__":
    rpc_py = None
    total_size = 64
    block_size = 512
    fail_count=0
    
    if len(sys.argv) == 2:
        rpc_py = sys.argv [1]
    elif len(sys.argv) == 4:
        rpc_py = sys.argv [1]
        total_size = int(sys.argv[2])
        block_size = int(sys.argv[3])
    else:
        print("Invalid argument")
        
    tc = Test_Cases(rpc_py)
    try:
        ### positive tests
        fail_count += tc.test_case1(total_size,block_size)
        fail_count += tc.test_case2(total_size,block_size)
        fail_count += tc.test_case3(total_size,block_size)
        fail_count += tc.test_case4(total_size,block_size)
        fail_count += tc.test_case5(total_size,block_size)
        fail_count += tc.test_case6(total_size,block_size)
        fail_count += tc.test_case7(total_size,block_size)
        fail_count += tc.test_case8(total_size,block_size)

        ### negative tests
        fail_count += tc.test_case9()
        fail_count += tc.test_case10(total_size,block_size)
        fail_count += tc.test_case11()
        fail_count += tc.test_case12(total_size,block_size)
        fail_count += tc.test_case13()
        fail_count += tc.test_case14(total_size,block_size)
        fail_count += tc.test_case15()
        fail_count += tc.test_case16(total_size,block_size)
        fail_count += tc.test_case17(total_size,block_size)
        fail_count += tc.test_case18(total_size,block_size)
        
        if not fail_count:
            print("All test case: PASSED")
    except:
        print("Failed tests")
        raise
