#!/usr/bin/env python
import io
import sys

import signal
from time import sleep

from Commands_Rpc_Lib import Commands_Rpc
from os import *



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
        10: 'construct_lvs_positive',
        11: 'construct_lvs_on_bdev_twic_negative',
        12: 'construct_logical_volume_nonexistent_lvs_uuid',
        13: 'construct_logical_volumes_on_busy_bdev',
        14: 'resize_logical_volume_nonexistent_logical_volume',
        15: 'resize_logical_volume_with_size_out_of_range',
        16: 'destroy_lvol_store_nonexistent_lvs_uuid',
        17: 'destroy_lvol_store_nonexistent_bdev',
        18: 'nasted construct_logical_volume_nonexistent',
        19: 'nasted construct_logical_volume_negative',
        20: 'nasted construct_logical_volume_negative',
        21: 'nasted construct_logical_volume_negative',
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
    def __init__(self, rpc_py, total_size, block_size):
        self.c = Commands_Rpc(rpc_py)
        self.total_size = total_size
        self.block_size = block_size
    ### positive tests
    def test_case1(self):
        header(1)
        base_name = self.c.construct_malloc_bdev(self.total_size,
                                                 self.block_size)
        uuid_store = self.c.construct_lvol_store(base_name)
        fail_count = self.c.check_get_lvol_stores(base_name, uuid_store)
        self.c.destroy_lvol_store(uuid_store)
        self.c.delete_bdev(base_name)
        footer(1)
        return fail_count
    def test_case2(self):
        header(2)
        base_name = self.c.construct_malloc_bdev(self.total_size,
                                                 self.block_size)
        uuid_store = self.c.construct_lvol_store(base_name)
        fail_count = self.c.check_get_lvol_stores(base_name, uuid_store)
        uuid_bdev = self.c.construct_lvol_bdev(uuid_store, self.total_size-1)
        fail_count += self.c.check_get_bdevs_methods(uuid_bdev)
        self.c.destroy_lvol_store(uuid_store)
        self.c.delete_bdev(base_name)
        footer(2)
        return fail_count
    def test_case3(self):
        header(3)
        base_name = self.c.construct_malloc_bdev(self.total_size,
                                                 self.block_size)
        uuid_store = self.c.construct_lvol_store(base_name)
        fail_count = self.c.check_get_lvol_stores(base_name, uuid_store)
        size =((self.total_size-1)/4)
        for _ in range(4):
            uuid_bdev = self.c.construct_lvol_bdev(uuid_store, size)
            fail_count += self.c.check_get_bdevs_methods(uuid_bdev)
        self.c.destroy_lvol_store(uuid_store)
        self.c.delete_bdev(base_name)
        footer(3)
        return fail_count
    def test_case4(self):
        header(4)
        base_name = self.c.construct_malloc_bdev(self.total_size,
                                                 self.block_size)
        uuid_store = self.c.construct_lvol_store(base_name)
        fail_count = self.c.check_get_lvol_stores(base_name, uuid_store)
        uuid_bdev = self.c.construct_lvol_bdev(uuid_store, self.total_size-1)
        fail_count += self.c.check_get_bdevs_methods(uuid_bdev)
        self.c.resize_lvol_bdev(uuid_bdev, self.total_size/2)
        self.c.resize_lvol_bdev(uuid_bdev, self.total_size/4)
        self.c.resize_lvol_bdev(uuid_bdev, self.total_size-1)
        self.c.destroy_lvol_store(uuid_store)
        self.c.delete_bdev(base_name)
        footer(4)
        return fail_count
    def test_case5(self):
        header(5)
        base_name = self.c.construct_malloc_bdev(self.total_size,
                                                 self.block_size)
        uuid_store = self.c.construct_lvol_store(base_name)
        fail_count = self.c.check_get_lvol_stores(base_name, uuid_store)
        self.c.destroy_lvol_store(uuid_store)
        fail_count += self.c.check_get_lvol_stores(None, None)
        self.c.delete_bdev(base_name)
        footer(5)
        return fail_count
    def test_case6(self):
        header(6)
        base_name = self.c.construct_malloc_bdev(self.total_size,
                                                 self.block_size)
        uuid_store = self.c.construct_lvol_store(base_name)
        fail_count = self.c.check_get_lvol_stores(base_name, uuid_store)
        uuid_bdev = self.c.construct_lvol_bdev(uuid_store, self.total_size-1)
        fail_count += self.c.check_get_bdevs_methods(uuid_bdev)
        self.c.destroy_lvol_store(uuid_store)
        fail_count += self.c.check_get_lvol_stores(None, None)
        self.c.delete_bdev(base_name)
        footer(6)
        return fail_count
    def test_case7(self):
        header(7)
        base_name = self.c.construct_malloc_bdev(self.total_size,
                                                 self.block_size)
        uuid_store = self.c.construct_lvol_store(base_name)
        fail_count = self.c.check_get_lvol_stores(base_name, uuid_store)
        size =((self.total_size - 1) / 4)
        for _ in range(4):
            uuid_bdev = self.c.construct_lvol_bdev(uuid_store, size)
            fail_count += self.c.check_get_bdevs_methods(uuid_bdev)
        self.c.destroy_lvol_store(uuid_store)
        fail_count += self.c.check_get_lvol_stores(None, None)
        self.c.delete_bdev(base_name)
        footer(7)
        return fail_count
    def test_case8(self):
        header(8)
        base_name = self.c.construct_malloc_bdev(self.total_size,
                                                 self.block_size)
        uuid_store = self.c.construct_lvol_store(base_name)
        fail_count = self.c.check_get_lvol_stores(base_name, uuid_store)
        uuid_bdev = self.c.construct_lvol_bdev(uuid_store, self.total_size-1)
        fail_count += self.c.check_get_bdevs_methods(uuid_bdev)
        uuid_store2 = self.c.construct_lvol_store(uuid_bdev)
        fail_count += self.c.check_get_lvol_stores(uuid_bdev, uuid_store2)
        self.c.delete_bdev(base_name)
        footer(8)
        return fail_count
    def test_case9(self):
        header(9)
        base_name = self.c.construct_malloc_bdev(self.total_size,
                                                 self.block_size)
        uuid_store = self.c.construct_lvol_store(base_name)
        fail_count = self.c.check_get_lvol_stores(base_name, uuid_store)
        size = ((self.total_size - 1) / 4)
        uuid_bdev = self.c.construct_lvol_bdev(uuid_store, size)
        fail_count += self.c.check_get_bdevs_methods(uuid_bdev)
        self.c.resize_lvol_bdev(uuid_bdev, size + 1)
        self.c.resize_lvol_bdev(uuid_bdev, size * 2)
        self.c.resize_lvol_bdev(uuid_bdev, size * 3)
        self.c.resize_lvol_bdev(uuid_bdev, (size * 4) - 1)
        self.c.resize_lvol_bdev(uuid_bdev, 0)
        self.c.destroy_lvol_store(uuid_store)
        self.c.delete_bdev(base_name)
        footer(9)
        return fail_count
    ### negative tests
    def test_case10(self):
        bad_uuid="123654789"
        header(10)
        try:
            self.c.construct_lvol_store(bad_uuid)
            print("FAILED: RPC COMMAND: construct_lvol_store")
            return 1
        except:
            pass
        footer(10)
        return 0
    def test_case11(self):
        header(11)
        base_name = self.c.construct_malloc_bdev(self.total_size,
                                                 self.block_size)
        uuid_store = self.c.construct_lvol_store(base_name)
        fail_count = self.c.check_get_lvol_stores(base_name, uuid_store)
        try:
            self.c.construct_lvol_store(base_name)
            print("FAILED: RPC COMMAND: construct_lvol_store")
            return 1
        except:
            pass
        self.c.destroy_lvol_store(uuid_store)
        self.c.delete_bdev(base_name)
        footer(11)
        return fail_count
    def test_case12(self):
        bad_uuid = "123654789"
        header(12)
        try:
            self.c.construct_lvol_bdev(bad_uuid, 32)
            print("PASSED: RPC COMMAND: construct_lvol_bdev")
            return 1
        except:
            pass
        footer(12)
        return 0
    def test_case13(self):
        header(13)
        base_name = self.c.construct_malloc_bdev(self.total_size,
                                                 self.block_size)
        uuid_store = self.c.construct_lvol_store(base_name)
        fail_count = self.c.check_get_lvol_stores(base_name, uuid_store)
        uuid_bdev = self.c.construct_lvol_bdev(uuid_store, self.total_size - 1)
        fail_count += self.c.check_get_bdevs_methods(uuid_bdev)
        try:
            self.c.construct_lvol_bdev(uuid_store, self.total_size - 1)
            print("FAILED: RPC COMMAND: construct_lvol_bdev")
            return 1
        except:
            pass
        self.c.destroy_lvol_store(uuid_store)
        self.c.delete_bdev(base_name)
        footer(13)
        return fail_count
    def test_case14(self):
        header(14)
        try:
            self.c.resize_lvol_bdev("1234", 16)
            print("FAILED: RPC COMMAND: resize_lvol_bdev")
            return 1
        except:
            pass
        footer(14)
        return 0
    def test_case15(self):
        header(15)
        base_name = self.c.construct_malloc_bdev(self.total_size,
                                                 self.block_size)
        uuid_store = self.c.construct_lvol_store(base_name)
        fail_count = self.c.check_get_lvol_stores(base_name, uuid_store)
        uuid_bdev = self.c.construct_lvol_bdev(uuid_store, self.total_size-1)
        fail_count += self.c.check_get_bdevs_methods(uuid_bdev)
        try:
            self.c.resize_lvol_bdev(uuid_bdev, self.total_size+1)
            print("FAILED: RPC COMMAND: resize_lvol_bdev - full size of bdev")
            return 1
        except:
            pass
        self.c.destroy_lvol_store(uuid_store)
        self.c.delete_bdev(base_name)
        footer(15)
        return fail_count
    def test_case16(self):
        bad_uuid = "123654789"
        header(16)
        try:
            self.c.destroy_lvol_store(bad_uuid)
            print("FAILED: RPC COMMAND: destroy_lvol_store")
            exit()
        except:
            pass
        footer(16)
        return 0
    def test_case17(self):
        header(17)
        base_name = self.c.construct_malloc_bdev(self.total_size,
                                                 self.block_size)
        uuid_store = self.c.construct_lvol_store(base_name)
        fail_count = self.c.check_get_lvol_stores(base_name, uuid_store)
        self.c.delete_bdev(base_name)
        try:
            self.c.destroy_lvol_store(uuid_store)
            print("FAILED: RPC COMMAND: destroy_lvol_store")
            return 1
        except:
            pass
        footer(17)
        return fail_count
    def test_case18(self):
        header(18)
        base_name = self.c.construct_malloc_bdev(self.total_size,
                                                 self.block_size)
        uuid_store = self.c.construct_lvol_store(base_name)
        fail_count = self.c.check_get_lvol_stores(base_name, uuid_store)
        uuid_bdev = self.c.construct_lvol_bdev(uuid_store, self.total_size-1)
        fail_count += self.c.check_get_bdevs_methods(uuid_bdev)
        try:
            uuid_store2 = self.c.construct_lvol_store(base_name)
            print("FAILED: RPC COMMAND: construct_lvol_store "
                  "{uuid}\n".format(uuid=uuid_store2))
            return 1
        except:
            pass
        self.c.delete_bdev(base_name)
        footer(18)
        return fail_count
    def test_case19(self):
        header(19)
        base_name = self.c.construct_malloc_bdev(self.total_size,
                                                 self.block_size)
        uuid_store = self.c.construct_lvol_store(base_name)
        fail_count = self.c.check_get_lvol_stores(base_name, uuid_store)
        uuid_bdev = self.c.construct_lvol_bdev(uuid_store, self.total_size-1)
        fail_count += self.c.check_get_bdevs_methods(uuid_bdev)
        uuid_store2 = self.c.construct_lvol_store(uuid_bdev)
        fail_count += self.c.check_get_lvol_stores(uuid_bdev, uuid_store2)
        try:
            self.c.destroy_lvol_store(uuid_store)
            print("FAILED: RPC COMMAND: destroy_lvol_store\n")
            return 1
        except:
            pass
        self.c.delete_bdev(base_name)
        footer(19)
        return fail_count
    def test_case20(self):
        header(20)
        base_name = self.c.construct_malloc_bdev(self.total_size,
                                                 self.block_size)
        uuid_store = self.c.construct_lvol_store(base_name)
        output = self.c.get_lvol_stores()
        if not output:
            print("FAILED: RPC COMMAND: get_lvol_storese response: "
                  "{uuid}\n".format(uuid=uuid_store))
            return 1
        self.c.destroy_lvol_store(uuid_store)
        self.c.delete_bdev(base_name)
        footer(20)
        return 0
    def test_case21(self):
        header(21)
        print self.block_size
        print self.total_size
        base_name = self.c.construct_malloc_bdev(self.total_size,
                                                 self.block_size)
        uuid_store = self.c.construct_lvol_store(base_name)
        fail_count = self.c.check_get_lvol_stores(base_name, uuid_store)
        pid_path =  path.abspath('bdev_io.pid')
        with io.open(pid_path, 'r') as bdev_io_pid:
            pid = (bdev_io_pid.readline()).split()
            if pid[0]:
                try:
                    kill(int(pid[0]), signal.SIGTERM)
                    sleep(5)
                except:
                    return 1
        footer(21)
        return fail_count
if __name__ == "__main__":
    rpc_py = None
    total_size = 64
    block_size = 512
    fail_count=0
    type_test = 1
    if len(sys.argv) == 2:
        rpc_py = sys.argv [1]
    elif len(sys.argv) == 5:
        rpc_py = sys.argv [1]
        total_size = int(sys.argv[2])
        block_size = int(sys.argv[3])
        type_test = int(sys.argv[4])
    else:
        print("Invalid argument")
    tc = Test_Cases(rpc_py,total_size,block_size)
    try:
        ### positive tests
        if  type_test == 1:
            fail_count += tc.test_case1()
            fail_count += tc.test_case2()
            fail_count += tc.test_case3()
            fail_count += tc.test_case4()
            fail_count += tc.test_case5()
            fail_count += tc.test_case6()
            fail_count += tc.test_case7()
            fail_count += tc.test_case8()
            fail_count += tc.test_case9()
        ### negative tests
        elif type_test == 2:
            fail_count += tc.test_case10()
            fail_count += tc.test_case11()
            fail_count += tc.test_case12()
            fail_count += tc.test_case13()
            fail_count += tc.test_case14()
            fail_count += tc.test_case15()
            fail_count += tc.test_case16()
            fail_count += tc.test_case17()
            fail_count += tc.test_case18()
            fail_count += tc.test_case19()
            fail_count += tc.test_case20()
        ### sigterm test
        elif type_test == 3:
            fail_count += tc.test_case21()
        else:
            print("ERROR: Invalid argument")
            fail_count += 1
        if not fail_count:
            print("All test case: PASSED")
    except:
        print("Failed tests")
        raise
