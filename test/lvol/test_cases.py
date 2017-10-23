#!/usr/bin/env python
import io
import sys
import random
import signal
import subprocess
import pprint

from errno import ESRCH
from os import kill, path, unlink, path, listdir, remove
from rpc_commands_lib import Commands_Rpc
from time import sleep
from uuid import uuid4


def test_counter():
    '''
    :return: the number of tests
    '''
    return 24


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
        13: 'construct_lvol_bdev_on_full_lvol_store',
        14: 'resize_logical_volume_nonexistent_logical_volume',
        15: 'resize_logical_volume_with_size_out_of_range',
        16: 'destroy_lvol_store_nonexistent_lvs_uuid',
        17: 'destroy_lvol_store_nonexistent_bdev',
        18: 'nested_construct_lvol_bdev_on_full_lvol_store',
        19: 'nested_destroy_logical_volume_positive',
        20: 'delete_bdev_positive',
        21: 'construct_lvs_with_cluster_sz_out_of_range_max',
        22: 'construct_lvs_with_cluster_sz_out_of_range_min',
        23: 'tasting_positive',
        24: 'SIGTERM',
    }
    print("========================================================")
    print("Test Case {num}: Start".format(num=num))
    print("Test Name: {name}".format(name=test_name[num]))
    print("========================================================")

def footer(num):
    print("Test Case {num}: END\n".format(num=num))
    print("========================================================")

class TestCases(object):

    def __init__(self, rpc_py, total_size, block_size, cluster_size, base_dir_path, app_path):
        self.c = Commands_Rpc(rpc_py)
        self.total_size = total_size
        self.block_size = block_size
        self.cluster_size = cluster_size
        self.path = base_dir_path
        self.app_path = app_path
        self.lvs_name = "lvs_test"
        self.lbd_name = "lbd_test"

    def _gen_lvs_uudi(self):
        return str(uuid4())

    def _gen_lvb_uudi(self):
        return "_".join([str(uuid4()), str(random.randrange(9999999999))])

    def _stop_vhost(self, pid_path):
        with io.open(pid_path, 'r') as vhost_pid:
            pid = int(vhost_pid.readline())
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
                        return 1
                else:
                    return 1
            else:
                return 1
        return 0

    def _start_vhost(self, vhost_path, config_path, pid_path):
        subprocess.call("{app} -c {config} -f "
                        "{pid} &".format(app=vhost_path,
                                         config=config_path,
                                         pid=pid_path), shell=True)
        for timeo in range(10):
            if timeo == 9:
                print("ERROR: Timeout on waiting for app start")
                return 1
            if not path.exists(pid_path):
                print("Waiting")
                sleep(1)
                continue
            else:
                break

        # Wait for RPC to open
        sleep(15)

        with io.open(pid_path, 'r') as vhost_pid:
            pid = int(vhost_pid.readline())
            if not pid:
                return 1
        return 0

    # positive tests
    def test_case1(self):
        header(1)
        base_name = self.c.construct_malloc_bdev(self.total_size,
                                                 self.block_size)
        uuid_store = self.c.construct_lvol_store(base_name,
                                                 self.lvs_name,
                                                 self.cluster_size)
        fail_count = self.c.check_get_lvol_stores(base_name, uuid_store,
                                                  self.cluster_size)
        self.c.destroy_lvol_store(uuid_store)
        self.c.delete_bdev(base_name)
        fail_count += self.c.check_get_lvol_stores("", "", "")
        footer(1)
        return fail_count

    def test_case2(self):
        header(2)
        base_name = self.c.construct_malloc_bdev(self.total_size,
                                                 self.block_size)
        uuid_store = self.c.construct_lvol_store(base_name,
                                                 self.lvs_name,
                                                 self.cluster_size)
        fail_count = self.c.check_get_lvol_stores(base_name, uuid_store,
                                                  self.cluster_size)
        uuid_bdev = self.c.construct_lvol_bdev(uuid_store,
                                               self.lbd_name,
                                               self.total_size - 1)
        fail_count += self.c.check_get_bdevs_methods(uuid_bdev,
                                                     self.total_size - 1)
        self.c.delete_bdev(uuid_bdev)
        self.c.destroy_lvol_store(uuid_store)
        self.c.delete_bdev(base_name)
        footer(2)
        return fail_count

    def test_case3(self):
        header(3)
        base_name = self.c.construct_malloc_bdev(self.total_size,
                                                 self.block_size)
        uuid_store = self.c.construct_lvol_store(base_name,
                                                 self.lvs_name,
                                                 self.cluster_size)
        fail_count = self.c.check_get_lvol_stores(base_name, uuid_store,
                                                  self.cluster_size)
        size = ((self.total_size - 1) / 4)

        uuid_bdevs = []
        for i in range(4):
            uuid_bdev = self.c.construct_lvol_bdev(uuid_store,
                                                   self.lbd_name + str(i),
                                                   size)
            uuid_bdevs.append(uuid_bdev)
            fail_count += self.c.check_get_bdevs_methods(uuid_bdev, size)

        for uuid_bdev in uuid_bdevs:
            self.c.delete_bdev(uuid_bdev)

        self.c.destroy_lvol_store(uuid_store)
        self.c.delete_bdev(base_name)
        footer(3)
        return fail_count

    def test_case4(self):
        header(4)
        base_name = self.c.construct_malloc_bdev(self.total_size,
                                                 self.block_size)
        uuid_store = self.c.construct_lvol_store(base_name,
                                                 self.lvs_name,
                                                 self.cluster_size)
        fail_count = self.c.check_get_lvol_stores(base_name, uuid_store,
                                                  self.cluster_size)
        # size is equal to one quarter of size malloc bdev
        uuid_bdev = self.c.construct_lvol_bdev(uuid_store,
                                               self.lbd_name,
                                               self.total_size / 4)
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

        self.c.delete_bdev(uuid_bdev)
        self.c.destroy_lvol_store(uuid_store)
        self.c.delete_bdev(base_name)
        footer(4)
        return fail_count

    def test_case5(self):
        header(5)
        base_name = self.c.construct_malloc_bdev(self.total_size,
                                                 self.block_size)
        uuid_store = self.c.construct_lvol_store(base_name,
                                                 self.lvs_name,
                                                 self.cluster_size)
        fail_count = self.c.check_get_lvol_stores(base_name, uuid_store,
                                                  self.cluster_size)
        self.c.destroy_lvol_store(uuid_store)
        fail_count += self.c.check_get_lvol_stores("", "", "")
        self.c.delete_bdev(base_name)
        footer(5)
        return fail_count

    def test_case6(self):
        header(6)
        base_name = self.c.construct_malloc_bdev(self.total_size,
                                                 self.block_size)
        uuid_store = self.c.construct_lvol_store(base_name,
                                                 self.lvs_name,
                                                 self.cluster_size)
        fail_count = self.c.check_get_lvol_stores(base_name, uuid_store,
                                                  self.cluster_size)
        uuid_bdev = self.c.construct_lvol_bdev(uuid_store,
                                               self.lbd_name,
                                               self.total_size - 1)
        fail_count += self.c.check_get_bdevs_methods(uuid_bdev,
                                                     self.total_size - 1)
        if self.c.destroy_lvol_store(uuid_store) != 0:
            fail_count += 1

        fail_count += self.c.check_get_lvol_stores("", "", "")
        self.c.delete_bdev(base_name)
        footer(6)
        return fail_count

    def test_case7(self):
        header(7)
        base_name = self.c.construct_malloc_bdev(self.total_size,
                                                 self.block_size)
        uuid_store = self.c.construct_lvol_store(base_name,
                                                 self.lvs_name,
                                                 self.cluster_size)
        fail_count = self.c.check_get_lvol_stores(base_name, uuid_store,
                                                  self.cluster_size)
        size = ((self.total_size - 1) / 4)

        for i in range(4):
            uuid_bdev = self.c.construct_lvol_bdev(uuid_store,
                                                   self.lbd_name + str(i),
                                                   size)
            fail_count += self.c.check_get_bdevs_methods(uuid_bdev, size)

        self.c.destroy_lvol_store(uuid_store)
        fail_count += self.c.check_get_lvol_stores("", "", "")
        self.c.delete_bdev(base_name)
        footer(7)
        return fail_count

    def test_case8(self):
        print("Test of this feature not yet implemented.")
        pass
        return 0

    def test_case9(self):
        header(9)
        base_name = self.c.construct_malloc_bdev(self.total_size,
                                                 self.block_size)
        uuid_store = self.c.construct_lvol_store(base_name,
                                                 self.lvs_name,
                                                 self.cluster_size)
        fail_count = self.c.check_get_lvol_stores(base_name, uuid_store,
                                                  self.cluster_size)
        size = ((self.total_size - 1) / 4)
        uuid_bdev = self.c.construct_lvol_bdev(uuid_store,
                                               self.lbd_name,
                                               size)
        fail_count += self.c.check_get_bdevs_methods(uuid_bdev,
                                                     size)

        # TODO: Improve resize_lvol_bdev tests to verify if bdev was actually
        # correctly resized
        fail_count += self.c.resize_lvol_bdev(uuid_bdev, size + 1)
        fail_count += self.c.resize_lvol_bdev(uuid_bdev, size * 2)
        fail_count += self.c.resize_lvol_bdev(uuid_bdev, size * 3)
        fail_count += self.c.resize_lvol_bdev(uuid_bdev, (size * 4) - 1)
        fail_count += self.c.resize_lvol_bdev(uuid_bdev, 0)

        self.c.destroy_lvol_store(uuid_store)
        fail_count += self.c.check_get_lvol_stores("", "", "")
        self.c.delete_bdev(base_name)
        footer(9)
        return fail_count

    # negative tests
    def test_case10(self):
        header(10)
        fail_count = 0
        bad_bdev_id = random.randrange(999999999)
        if self.c.construct_lvol_store(bad_bdev_id,
                                       self.lvs_name,
                                       self.cluster_size) == 0:
            fail_count += 1
        footer(10)
        return fail_count

    def test_case11(self):
        header(11)
        base_name = self.c.construct_malloc_bdev(self.total_size,
                                                 self.block_size)
        uuid_store = self.c.construct_lvol_store(base_name,
                                                 self.lvs_name,
                                                 self.cluster_size)
        fail_count = self.c.check_get_lvol_stores(base_name, uuid_store,
                                                  self.cluster_size)
        if self.c.construct_lvol_store(base_name,
                                       self.lvs_name,
                                       self.cluster_size) == 0:
            fail_count += 1
        self.c.destroy_lvol_store(uuid_store)
        self.c.delete_bdev(base_name)
        footer(11)
        return fail_count

    def test_case12(self):
        header(12)
        fail_count = 0
        if self.c.construct_lvol_bdev(self._gen_lvs_uudi(),
                                      self.lbd_name,
                                      32) == 0:
            fail_count += 1
        footer(12)
        return fail_count

    def test_case13(self):
        header(13)
        base_name = self.c.construct_malloc_bdev(self.total_size,
                                                 self.block_size)
        uuid_store = self.c.construct_lvol_store(base_name,
                                                 self.lvs_name,
                                                 self.cluster_size)
        fail_count = self.c.check_get_lvol_stores(base_name, uuid_store,
                                                  self.cluster_size)
        uuid_bdev = self.c.construct_lvol_bdev(uuid_store,
                                               self.lbd_name,
                                               self.total_size - 1)
        fail_count += self.c.check_get_bdevs_methods(uuid_bdev,
                                                     self.total_size - 1)
        if self.c.construct_lvol_bdev(uuid_store,
                                      self.lbd_name + "_1",
                                      self.total_size - 1) == 0:
            fail_count += 1

        self.c.delete_bdev(uuid_bdev)
        self.c.destroy_lvol_store(uuid_store)
        self.c.delete_bdev(base_name)
        footer(13)
        return fail_count

    def test_case14(self):
        header(14)
        fail_count = 0
        if self.c.resize_lvol_bdev(self._gen_lvb_uudi(), 16) == 0:
            fail_count += 1
        footer(14)
        return fail_count

    def test_case15(self):
        header(15)
        base_name = self.c.construct_malloc_bdev(self.total_size,
                                                 self.block_size)
        uuid_store = self.c.construct_lvol_store(base_name,
                                                 self.lvs_name,
                                                 self.cluster_size)
        fail_count = self.c.check_get_lvol_stores(base_name, uuid_store,
                                                  self.cluster_size)
        uuid_bdev = self.c.construct_lvol_bdev(uuid_store,
                                               self.lbd_name,
                                               self.total_size - 1)
        fail_count += self.c.check_get_bdevs_methods(uuid_bdev,
                                                     self.total_size - 1)
        if self.c.resize_lvol_bdev(uuid_bdev, self.total_size + 1) == 0:
            fail_count += 1

        self.c.delete_bdev(uuid_bdev)
        self.c.destroy_lvol_store(uuid_store)
        self.c.delete_bdev(base_name)
        footer(15)
        return fail_count

    def test_case16(self):
        header(16)
        fail_count = 0
        if self.c.destroy_lvol_store(self._gen_lvs_uudi()) == 0:
            fail_count += 1
        footer(16)
        return fail_count

    def test_case17(self):
        header(17)
        base_name = self.c.construct_malloc_bdev(self.total_size,
                                                 self.block_size)
        uuid_store = self.c.construct_lvol_store(base_name,
                                                 self.lvs_name,
                                                 self.cluster_size)
        fail_count = self.c.check_get_lvol_stores(base_name, uuid_store,
                                                  self.cluster_size)

        if self.c.delete_bdev(base_name) != 0:
            fail_count += 1

        if self.c.destroy_lvol_store(uuid_store) == 0:
            fail_count += 1

        footer(17)
        return fail_count

    def test_case18(self):
        print("Test of this feature not yet implemented.")
        pass
        return 0

    def test_case19(self):
        print("Test of this feature not yet implemented.")
        pass
        return 0

    def test_case20(self):
        header(20)
        base_name = self.c.construct_malloc_bdev(self.total_size,
                                                 self.block_size)
        uuid_store = self.c.construct_lvol_store(base_name,
                                                 self.lvs_name,
                                                 self.cluster_size)
        fail_count = self.c.check_get_lvol_stores(base_name, uuid_store,
                                                  self.cluster_size)
        self.c.delete_bdev(base_name)
        fail_count += self.c.check_get_lvol_stores("", "", "")
        footer(20)
        return fail_count

    def test_case21(self):
        header(21)
        fail_count = 0
        base_name = self.c.construct_malloc_bdev(self.total_size,
                                                 self.block_size)
        if self.c.construct_lvol_store(base_name,
                                       self.lvs_name,
                                       (self.total_size * 1024 * 1024) + 1) == 0:
            fail_count += 1
        footer(21)
        return fail_count

    def test_case22(self):
        header(22)
        fail_count = 0
        base_name = self.c.construct_malloc_bdev(self.total_size,
                                                 self.block_size)
        if self.c.construct_lvol_store(base_name,
                                       self.lvs_name, 0) == 0:
            fail_count += 1
        footer(22)
        return fail_count

    def test_case23(self):
        header(23)
        fail_count = 0
        uuid_bdevs = []
        base_name = "Nvme0n1"

        # TODO: Replace fixed size with calculated 10% of total NVMe disk size
        size = 1000

        base_path = path.dirname(sys.argv[0])
        vhost_path = path.join(self.app_path, 'vhost')
        config_path = path.join(base_path, 'vhost.conf')
        pid_path = path.join(base_path, 'vhost.pid')

        # Create initial configuration on running vhost instance
        # create lvol store, create 5 bdevs
        # save info of all lvs and lvol bdevs
        uuid_store = self.c.construct_lvol_store(base_name,
                                                 self.lvs_name,
                                                 self.cluster_size)
        fail_count += self.c.check_get_lvol_stores(base_name,
                                                   uuid_store,
                                                   self.cluster_size)

        for i in range(5):
            uuid_bdev = self.c.construct_lvol_bdev(uuid_store,
                                                   self.lbd_name + str(i),
                                                   size)
            uuid_bdevs.append(uuid_bdev)
            fail_count += self.c.check_get_bdevs_methods(uuid_bdev, size)

        old_bdevs = sorted(self.c.get_lvol_bdevs(), key=lambda x: x["name"])
        old_stores = self.c.get_lvol_stores()

        # Shut down vhost instance and restart with new instance
        fail_count += self._stop_vhost(pid_path)
        remove(pid_path)
        fail_count += self._start_vhost(vhost_path, config_path, pid_path)

        # Check if configuration was properly loaded after tasting
        # get all info all lvs and lvol bdevs, compare with previous info
        new_bdevs = sorted(self.c.get_lvol_bdevs(), key=lambda x: x["name"])
        new_stores = self.c.get_lvol_stores()

        if old_stores != new_stores:
            fail_count += 1
            print("ERROR: old and loaded lvol store is not the same")
            print("DIFF:")
            print(old_stores)
            print(new_stores)

        if len(old_bdevs) != len(new_bdevs):
            fail_count += 1
            print("ERROR: old and loaded lvol bdev list is not equal")

        for o, n in zip(old_bdevs, new_bdevs):
            if o != n:
                fail_count += 1
                print("ERROR: old and loaded lvol bdev is not the same")
                print("DIFF:")
                pprint.pprint([o, n])

        # Try modifying loaded configuration
        # Add some lvol bdevs to existing lvol store then
        # remove all lvol configuration and re-create it again
        for i in range(5, 10):
            uuid_bdev = self.c.construct_lvol_bdev(uuid_store,
                                                   self.lbd_name + str(i),
                                                   size)
            uuid_bdevs.append(uuid_bdev)
            fail_count += self.c.check_get_bdevs_methods(uuid_bdev, size)

        for uuid_bdev in uuid_bdevs:
            self.c.delete_bdev(uuid_bdev)

        if self.c.destroy_lvol_store(uuid_store) != 0:
            fail_count += 1

        uuid_bdevs = []

        uuid_store = self.c.construct_lvol_store(base_name,
                                                 self.lvs_name,
                                                 self.cluster_size)
        fail_count += self.c.check_get_lvol_stores(base_name,
                                                   uuid_store,
                                                   self.cluster_size)

        for i in range(10):
            uuid_bdev = self.c.construct_lvol_bdev(uuid_store,
                                                   self.lbd_name + str(i),
                                                   size)
            uuid_bdevs.append(uuid_bdev)
            fail_count += self.c.check_get_bdevs_methods(uuid_bdev, size)

        if self.c.destroy_lvol_store(uuid_store) != 0:
            fail_count += 1

        footer(23)
        return fail_count

    def test_case24(self):
        header(24)
        pid_path = path.join(self.path, 'vhost.pid')

        base_name = self.c.construct_malloc_bdev(self.total_size,
                                                 self.block_size)
        uuid_store = self.c.construct_lvol_store(base_name,
                                                 self.lvs_name,
                                                 self.cluster_size)
        fail_count = self.c.check_get_lvol_stores(base_name, uuid_store,
                                                  self.cluster_size)

        fail_count += self._stop_vhost(pid_path)
        footer(24)
        return fail_count
