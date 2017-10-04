#!/usr/bin/env python
import io
import random
import signal

from errno import ESRCH
from os import kill, path
from rpc_commands_lib import Commands_Rpc
from time import sleep

def test_counter():
    '''
    :return: the number of tests
    '''
    return 21

def header(num):
    test_name = {
        1: 'construct_lvs_positive',
        2: 'construct_logical_volume_positive',
        3: 'construct_multi_logical_volumes_positive',
        4: 'resize_lvol_bdev_positive',
        5: 'destroy_lvol_store_positive',
        6: 'destroy_lvol_store_with_lvol_bdev_positive',
        7: 'destroy_multi_logical_volumes_positive',
        8: 'nested_construct_logical_volume_positive',
        9: 'destroy_after_resize_lvol_bdev_positive',
        10: 'construct_lvs_nonexistent_bdev',
        11: 'construct_lvs_on_bdev_twic_negative',
        12: 'construct_logical_volume_nonexistent_lvs_uuid',
        13: 'construct_logical_volumes_on_busy_bdev',
        14: 'resize_logical_volume_nonexistent_logical_volume',
        15: 'resize_logical_volume_with_size_out_of_range',
        16: 'destroy_lvol_store_nonexistent_lvs_uuid',
        17: 'destroy_lvol_store_nonexistent_bdev',
        18: 'nested_construct_logical_volume__on_busy_bdev',
        19: 'nested_destroy_logical_volume_positive',
        20: 'delete_bdev_positive',
        21: 'SIGTERM',
        22: 'SIGTERM_nested_lvol'
    }
    print("========================================================")
    print("Test Case{num}: Start".format(num=num))
    print("Test Name: {name}".format(name=test_name[num]))
    print("========================================================")

def footer(num):
    print("Test Case{num}: END\n".format(num=num))
    print("========================================================")

class TestCases(object):
    def __init__(self, rpc_py, total_size, block_size, base_dir_path):
        self.c = Commands_Rpc(rpc_py)
        self.total_size = total_size
        self.block_size = block_size
        self.path = base_dir_path

    # positive tests
    def test_case1(self):
        header(1)
        base_name = self.c.construct_malloc_bdev(self.total_size,
                                                 self.block_size)
        uuid_store = self.c.construct_lvol_store(base_name, self.total_size))
        fail_count = self.c.check_get_lvol_stores(base_name, uuid_store)
        self.c.destroy_lvol_store(uuid_store)
        self.c.delete_bdev(base_name)
        fail_count += self.c.check_get_lvol_stores("", "")
        footer(1)
        return fail_count

    def test_case2(self):
        header(2)
        base_name = self.c.construct_malloc_bdev(self.total_size,
                                                 self.block_size)
        uuid_store = self.c.construct_lvol_store(base_name, self.total_size))
        fail_count = self.c.check_get_lvol_stores(base_name, uuid_store)
        uuid_bdev = self.c.construct_lvol_bdev(uuid_store, self.total_size - 1)
        fail_count += self.c.check_get_bdevs_methods(uuid_bdev,
                                                     self.total_size - 1)
        self.c.destroy_lvol_store(uuid_store)
        self.c.delete_bdev(base_name)
        footer(2)
        return fail_count

    def test_case3(self):
        header(3)
        base_name = self.c.construct_malloc_bdev(self.total_size,
                                                 self.block_size)
        uuid_store = self.c.construct_lvol_store(base_name, self.total_size))
        fail_count = self.c.check_get_lvol_stores(base_name, uuid_store)
        size = ((self.total_size - 1) / 4)
        for _ in range(4):
            uuid_bdev = self.c.construct_lvol_bdev(uuid_store, size)
            fail_count += self.c.check_get_bdevs_methods(uuid_bdev, size)
        self.c.destroy_lvol_store(uuid_store)
        self.c.delete_bdev(base_name)
        footer(3)
        return fail_count

    def test_case4(self):
        header(4)
        base_name = self.c.construct_malloc_bdev(self.total_size,
                                                 self.block_size)
        uuid_store = self.c.construct_lvol_store(base_name, self.total_size))
        fail_count = self.c.check_get_lvol_stores(base_name, uuid_store)
        # size is equal to one quarter of size malloc bdev
        uuid_bdev = self.c.construct_lvol_bdev(uuid_store, self.total_size / 4)
        fail_count += self.c.check_get_bdevs_methods(uuid_bdev,
                                                     self.total_size / 4)
        # size is equal to half  of size malloc bdev
        self.c.resize_lvol_bdev(uuid_bdev, self.total_size / 2)
        fail_count += self.c.check_get_bdevs_methods(uuid_bdev,
                                                     self.total_size / 2)
        # size is smaller by 1 MB
        self.c.resize_lvol_bdev(uuid_bdev, self.total_size - 1)
        fail_count += self.c.check_get_bdevs_methods(uuid_bdev,
                                                     self.total_size - 1)
        # size is equal 0 MiB
        self.c.resize_lvol_bdev(uuid_bdev, 0)
        fail_count += self.c.check_get_bdevs_methods(uuid_bdev, 0)
        self.c.destroy_lvol_store(uuid_store)
        self.c.delete_bdev(base_name)
        footer(4)
        return fail_count

    def test_case5(self):
        header(5)
        base_name = self.c.construct_malloc_bdev(self.total_size,
                                                 self.block_size)
        uuid_store = self.c.construct_lvol_store(base_name, self.total_size))
        fail_count = self.c.check_get_lvol_stores(base_name, uuid_store)
        self.c.destroy_lvol_store(uuid_store)
        fail_count += self.c.check_get_lvol_stores("", "")
        self.c.delete_bdev(base_name)
        footer(5)
        return fail_count

    def test_case6(self):
        header(6)
        base_name = self.c.construct_malloc_bdev(self.total_size,
                                                 self.block_size)
        uuid_store = self.c.construct_lvol_store(base_name, self.total_size))
        fail_count = self.c.check_get_lvol_stores(base_name, uuid_store)
        uuid_bdev = self.c.construct_lvol_bdev(uuid_store, self.total_size - 1)
        fail_count += self.c.check_get_bdevs_methods(uuid_bdev,
                                                     self.total_size - 1)
        self.c.destroy_lvol_store(uuid_store)
        fail_count += self.c.check_get_lvol_stores("", "")
        self.c.delete_bdev(base_name)
        footer(6)
        return fail_count

    def test_case7(self):
        header(7)
        base_name = self.c.construct_malloc_bdev(self.total_size,
                                                 self.block_size)
        uuid_store = self.c.construct_lvol_store(base_name, self.total_size))
        fail_count = self.c.check_get_lvol_stores(base_name, uuid_store)
        size = ((self.total_size - 1) / 4)
        for _ in range(4):
            uuid_bdev = self.c.construct_lvol_bdev(uuid_store, size)
            fail_count += self.c.check_get_bdevs_methods(uuid_bdev, size)
        self.c.destroy_lvol_store(uuid_store)
        fail_count += self.c.check_get_lvol_stores("", "")
        self.c.delete_bdev(base_name)
        footer(7)
        return fail_count

    def test_case8(self):
        pass
        return 0

    def test_case9(self):
        header(9)
        base_name = self.c.construct_malloc_bdev(self.total_size,
                                                 self.block_size)
        uuid_store = self.c.construct_lvol_store(base_name, self.total_size))
        fail_count = self.c.check_get_lvol_stores(base_name, uuid_store)
        size = ((self.total_size - 1) / 4)
        uuid_bdev = self.c.construct_lvol_bdev(uuid_store, size)
        fail_count += self.c.check_get_bdevs_methods(uuid_bdev,
                                                     self.total_size - 1)
        self.c.resize_lvol_bdev(uuid_bdev, size + 1)
        self.c.resize_lvol_bdev(uuid_bdev, size * 2)
        self.c.resize_lvol_bdev(uuid_bdev, size * 3)
        self.c.resize_lvol_bdev(uuid_bdev, (size * 4) - 1)
        self.c.resize_lvol_bdev(uuid_bdev, 0)
        self.c.destroy_lvol_store(uuid_store)
        fail_count += self.c.check_get_lvol_stores("", "")
        self.c.destroy_lvol_store(uuid_store)
        footer(9)
        return fail_count

    # negative tests
    def test_case10(self):
        bad_bdev_uuid = random.randrange(999999999)
        header(10)
        try:
            self.c.construct_lvol_store(bad_bdev_uuid)
            print("FAILED: RPC COMMAND: construct_lvol_store")
            return fail_count + 1
        except:
            pass
        footer(10)
        return 0

    def test_case11(self):
        header(11)
        base_name = self.c.construct_malloc_bdev(self.total_size,
                                                 self.block_size)
        uuid_store = self.c.construct_lvol_store(base_name, self.total_size))
        fail_count = self.c.check_get_lvol_stores(base_name, uuid_store)
        try:
            self.c.construct_lvol_store(base_name, self.total_size))
            print("FAILED: RPC COMMAND: construct_lvol_store")
            return fail_count + 1
        except:
            pass
        self.c.destroy_lvol_store(uuid_store)
        self.c.delete_bdev(base_name)
        footer(11)
        return fail_count

    def test_case12(self):
        bad_bdev_uuid = random.randrange(999999999)
        header(12)
        try:
            self.c.construct_lvol_bdev(bad_bdev_uuid, 32)
            print("FAILED: RPC COMMAND: construct_lvol_bdev")
            return fail_count + 1
        except:
            pass
        footer(12)
        return 0

    def test_case13(self):
        header(13)
        base_name = self.c.construct_malloc_bdev(self.total_size,
                                                 self.block_size)
        uuid_store = self.c.construct_lvol_store(base_name, self.total_size))
        fail_count = self.c.check_get_lvol_stores(base_name, uuid_store)
        uuid_bdev = self.c.construct_lvol_bdev(uuid_store, self.total_size - 1)
        fail_count += self.c.check_get_bdevs_methods(uuid_bdev,
                                                     self.total_size - 1)
        try:
            self.c.construct_lvol_bdev(uuid_store, self.total_size - 1)
            print("FAILED: RPC COMMAND: construct_lvol_bdev")
            return fail_count + 1
        except:
            pass
        self.c.destroy_lvol_store(uuid_store)
        self.c.delete_bdev(base_name)
        footer(13)
        return fail_count

    def test_case14(self):
        header(14)
        bad_bdev_uuid = random.randrange(999999999)
        try:
            self.c.resize_lvol_bdev(bad_bdev_uuid, 16)
            print("FAILED: RPC COMMAND: resize_lvol_bdev")
            return fail_count + 1
        except:
            pass
        footer(14)
        return 0

    def test_case15(self):
        header(15)
        base_name = self.c.construct_malloc_bdev(self.total_size,
                                                 self.block_size)
        uuid_store = self.c.construct_lvol_store(base_name, self.total_size))
        fail_count = self.c.check_get_lvol_stores(base_name, uuid_store)
        uuid_bdev = self.c.construct_lvol_bdev(uuid_store, self.total_size - 1)
        fail_count += self.c.check_get_bdevs_methods(uuid_bdev,
                                                     self.total_size - 1)
        try:
            self.c.resize_lvol_bdev(uuid_bdev, self.total_size + 1)
            print("FAILED: RPC COMMAND: resize_lvol_bdev - full size of bdev")
            return fail_count + 1
        except:
            pass
        self.c.destroy_lvol_store(uuid_store)
        self.c.delete_bdev(base_name)
        footer(15)
        return fail_count

    def test_case16(self):
        bad_bdev_uuid = random.randrange(999999999)
        header(16)
        try:
            self.c.destroy_lvol_store(bad_bdev_uuid)
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
        uuid_store = self.c.construct_lvol_store(base_name, self.total_size)
        fail_count = self.c.check_get_lvol_stores(base_name, uuid_store)
        self.c.delete_bdev(base_name)
        try:
            self.c.destroy_lvol_store(uuid_store)
            print("FAILED: RPC COMMAND: destroy_lvol_store")
            return fail_count + 1
        except:
            pass
        footer(17)
        return fail_count




    def test_case19(self):
        header(19)
        base_name = self.c.construct_malloc_bdev(self.total_size,
                                                 self.block_size)
        uuid_store = self.c.construct_lvol_store(base_name, self.total_size)
        fail_count = self.c.check_get_lvol_stores(base_name, uuid_store)
        uuid_bdev = self.c.construct_lvol_bdev(uuid_store, self.total_size - 1)
        fail_count += self.c.check_get_bdevs_methods(uuid_bdev,
                                                     self.total_size - 1)
        uuid_store2 = self.c.construct_lvol_store(uuid_bdev)
        fail_count += self.c.check_get_lvol_stores(uuid_bdev, uuid_store2)
        try:
            self.c.destroy_lvol_store(uuid_store)
        except:
            return fail_count + 1
        self.c.delete_bdev(base_name)
        footer(19)
        return fail_count

    def test_case20(self):
        pass
        return 0

    def test_case21(self):
        header(21)
        print self.block_size
        print self.total_size
        base_name = self.c.construct_malloc_bdev(self.total_size,
                                                 self.block_size)
        uuid_store = self.c.construct_lvol_store(base_name, self.total_size)
        fail_count = self.c.check_get_lvol_stores(base_name, uuid_store)
        pid_path = path.join(self.path, 'nbd.pid')
        with io.open(pid_path, 'r') as nbd_pid:
            pid = int(nbd_pid.readline())
            if pid:
                try:
                    kill(pid, signal.SIGTERM)
                    for count in range(30):
                        sleep(1)
                        kill(pid, 0)
                except OSError, err:
                    if err.errno == ESRCH:
                        pass
                    else:
                        return fail_count + 1
                else:
                    return fail_count + 1
            else:
                return fail_count + 1
        footer(21)
        return fail_count

    def test_case22(self):
        header(22)
        base_name = self.c.construct_malloc_bdev(self.total_size,
                                                 self.block_size)
        uuid_store = self.c.construct_lvol_store(base_name, self.total_size))
        fail_count = self.c.check_get_lvol_stores(base_name, uuid_store)
        uuid_bdev = self.c.construct_lvol_bdev(uuid_store, self.total_size - 1)
        fail_count += self.c.check_get_bdevs_methods(uuid_bdev,
                                                     self.total_size - 1)
        uuid_store2 = self.c.construct_lvol_store(uuid_bdev, self.total_size - 1)
        fail_count += self.c.check_get_lvol_stores(uuid_bdev, uuid_store2)
        uuid_bdev2 = self.c.construct_lvol_bdev(uuid_store2,
                                                self.total_size - 1)
        fail_count += self.c.check_get_bdevs_methods(uuid_bdev2,
                                                     self.total_size - 1)
        pid_path = path.join(self.path, 'nbd.pid')
        with io.open(pid_path, 'r') as nbd_pid:
            pid = int(nbd_pid.readline())
            if pid:
                try:
                    kill(pid, signal.SIGTERM)
                    for count in range(30):
                        sleep(1)
                        kill(pid, 0)
                except OSError, err:
                    if err.errno == ESRCH:
                        pass
                    else:
                        return fail_count + 1
                else:
                    return fail_count + 1
            else:
                return fail_count + 1
        footer(22)
        return fail_count
