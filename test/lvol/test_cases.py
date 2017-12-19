#!/usr/bin/env python
import io
import time
import sys
import random
import signal
import subprocess
import pprint
import socket
import threading
import shutil
import fileinput

from errno import ESRCH
from os import kill, path, unlink, path, listdir, remove
from rpc_commands_lib import Commands_Rpc
from time import sleep
from uuid import uuid4

MEGABYTE = 1024 * 1024

def get_fio_cmd(nbd_disk, offset, size, rw, pattern, extra_params=""):
    fio_template = "fio --name=fio_test --filename=%(file)s --offset=%(offset)s --size=%(size)s"\
                   " --rw=%(rw)s --direct=1 %(extra_params)s %(pattern)s"
    pattern_template = ""
    if pattern:
        pattern_template = " --do_verify=1 --verify=pattern --verify_pattern=%s"\
                           " --verify_state_save=0" % pattern
    fio_cmd = fio_template % {"file": nbd_disk, "offset": offset, "size": size,
                              "rw": rw, "pattern": pattern_template,
                              "extra_params": extra_params}

    return fio_cmd


def run_fio(fio_cmd, expected_ret_value):
    try:
        output_fio = subprocess.check_output(fio_cmd, stderr=subprocess.STDOUT, shell=True)
        rv = 0
    except subprocess.CalledProcessError, ex:
        rv = 1
    except Exception as e:
        print("ERROR: Fio test ended with unexpected exception.")
        rv = 1
    if expected_ret_value == rv:
        return 0

    if rv == 0:
        print("ERROR: Fio test ended with unexpected success")
    else:
        print("ERROR: Fio test ended with unexpected failure")
    return 1


class FioThread(threading.Thread):
    def __init__(self, nbd_disk, offset, size, rw, pattern, expected_ret_value):
        super(FioThread, self).__init__()
        self.fio_cmd = get_fio_cmd(nbd_disk, offset, size, rw, pattern,
                                   extra_params="--time_based --runtime=5")
        self.rv = 1
        self.expected_ret_value = expected_ret_value

    def run(self):
        self.rv = run_fio(self.fio_cmd, self.expected_ret_value)


MEGABYTE = 1024 * 1024

def test_counter():
    '''
    :return: the number of tests
    '''
    return 37


def header(num):
    test_name = {
        1: 'construct_lvs_positive',
        50: 'construct_logical_volume_positive',
        51: 'construct_multi_logical_volumes_positive',
        52: 'construct_lvol_bdev_using_name_positive',
        53: 'construct_lvol_bdev_duplicate_names_positive',
        100: 'construct_logical_volume_nonexistent_lvs_uuid',
        101: 'construct_lvol_bdev_on_full_lvol_store',
        102: 'construct_lvol_bdev_name_twice',
        150: 'resize_lvol_bdev_positive',
        200: 'resize_logical_volume_nonexistent_logical_volume',
        201: 'resize_logical_volume_with_size_out_of_range',
        250: 'destroy_lvol_store_positive',
        251: 'destroy_lvol_store_use_name_positive',
        252: 'destroy_lvol_store_with_lvol_bdev_positive',
        253: 'destroy_multi_logical_volumes_positive',
        254: 'destroy_after_resize_lvol_bdev_positive',
        255: 'delete_lvol_store_persistent_positive',
        300: 'destroy_lvol_store_nonexistent_lvs_uuid',
        301: 'delete_lvol_store_underlying_bdev',
        350: 'nested_destroy_logical_volume_negative',
        400: 'nested_construct_logical_volume_positive',
        450: 'construct_lvs_nonexistent_bdev',
        451: 'construct_lvs_on_bdev_twice',
        452: 'construct_lvs_name_twice',
        500: 'nested_construct_lvol_bdev_on_full_lvol_store',
        550: 'delete_bdev_positive',
        600: 'construct_lvol_store_with_cluster_size_max',
        601: 'construct_lvol_store_with_cluster_size_min',
        650: 'thin_provisioning_check_space',
        651: 'thin_provisioning_read_empty_bdev',
        652: 'thin_provisionind_data_integrity_test',
        653: 'thin_provisioning_resize',
        654: 'thin_overprovisioning',
        655: 'thin_provisioning_filling_disks_less_than_lvs_size',
        700: 'tasting_positive',
        701: 'tasting_lvol_store_positive',
        750: 'snapshot_readonly',
        751: 'snapshot_compare_with_lvol_bdev',
        752: 'snapshot_during_io_traffic',
        753: 'snapshot_of_snapshot',
        754: 'clone_bdev_only',
        755: 'clone_writing_clone',
        10000: 'SIGTERM',
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
        self.vhost_config_path = path.join(path.dirname(sys.argv[0]), 'vhost.conf')

    def _gen_lvs_uudi(self):
        return str(uuid4())

    def _gen_lvb_uudi(self):
        return "_".join([str(uuid4()), str(random.randrange(9999999999))])

    def compare_two_disks(self, disk1, disk2, expected_ret_value):
        cmp_cmd = "cmp %s %s" % (disk1, disk2)
        try:
            process = subprocess.check_output(cmp_cmd, stderr=subprocess.STDOUT, shell=True)
            rv = 0
        except subprocess.CalledProcessError, ex:
            rv = 1
        except Exception as e:
            print("ERROR: Cmp ended with unexpected exception.")
            rv = 1

        if expected_ret_value == rv:
            return 0
        elif rv == 0:
            print("ERROR: Cmp ended with unexpected success")
        else:
            print("ERROR: Cmp ended with unexpected failure")

        return 1

    def run_fio_test(self, nbd_disk, offset, size, rw, pattern, expected_ret_value=0):
        fio_cmd = get_fio_cmd(nbd_disk, offset, size, rw, pattern)
        return run_fio(fio_cmd, expected_ret_value)

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
                print("Info: Waiting for PID file...")
                sleep(1)
                continue
            else:
                break

        # Wait for RPC to open
        sock = socket.socket(socket.AF_UNIX)
        for timeo in range(30):
            if timeo == 29:
                print("ERROR: Timeout on waiting for RPC start")
                return 1
            try:
                sock.connect("/var/tmp/spdk.sock")
                break
            except socket.error as e:
                print("Info: Waiting for RPC Unix socket...")
                sleep(1)
                continue
            else:
                sock.close()
                break

        with io.open(pid_path, 'r') as vhost_pid:
            pid = int(vhost_pid.readline())
            if not pid:
                return 1
        return 0

    def _find_traddress_for_nvme(self, nvme_name):
        with open(self.vhost_config_path) as file:
            for line in file:
                if nvme_name in line and "TransportID" in line:
                    for word in line.split(" "):
                        if word.startswith("traddr"):
                            return word.split(":", 1)[1].replace("\"", "")

        print("INFO: Traddr not found for Nvme {nvme}".format(nvme=nvme_name))
        return -1

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
        if self.c.check_get_lvol_stores("", "", "") == 1:
            fail_count += 1
        footer(1)
        return fail_count

    def test_case50(self):
        header(50)
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
        footer(50)
        return fail_count

    def test_case51(self):
        header(51)
        base_name = self.c.construct_malloc_bdev(self.total_size,
                                                 self.block_size)
        uuid_store = self.c.construct_lvol_store(base_name,
                                                 self.lvs_name,
                                                 self.cluster_size)
        fail_count = self.c.check_get_lvol_stores(base_name, uuid_store,
                                                  self.cluster_size)
        size = ((self.total_size - 1) / 4)

        for j in range(2):
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
        footer(51)
        return fail_count

    def test_case52(self):
        header(52)
        base_name = self.c.construct_malloc_bdev(self.total_size,
                                                 self.block_size)

        uuid_store = self.c.construct_lvol_store(base_name,
                                                 self.lvs_name,
                                                 self.cluster_size)

        fail_count = self.c.check_get_lvol_stores(base_name, uuid_store,
                                                  self.cluster_size)

        uuid_bdev = self.c.construct_lvol_bdev(self.lvs_name,
                                               self.lbd_name,
                                               self.total_size - 1)
        fail_count += self.c.check_get_bdevs_methods(uuid_bdev,
                                                     self.total_size - 1)

        fail_count += self.c.delete_bdev(uuid_bdev)
        fail_count += self.c.destroy_lvol_store(uuid_store)
        fail_count += self.c.delete_bdev(base_name)
        footer(52)
        return fail_count

    def test_case53(self):
        header(53)
        base_name_1 = self.c.construct_malloc_bdev(self.total_size,
                                                   self.block_size)
        base_name_2 = self.c.construct_malloc_bdev(self.total_size,
                                                   self.block_size)

        uuid_store_1 = self.c.construct_lvol_store(base_name_1,
                                                   self.lvs_name + "1",
                                                   self.cluster_size)
        uuid_store_2 = self.c.construct_lvol_store(base_name_2,
                                                   self.lvs_name + "2",
                                                   self.cluster_size)
        fail_count = self.c.check_get_lvol_stores(base_name_1, uuid_store_1,
                                                  self.cluster_size)
        fail_count = self.c.check_get_lvol_stores(base_name_2, uuid_store_2,
                                                  self.cluster_size)

        uuid_bdev_1 = self.c.construct_lvol_bdev(uuid_store_1,
                                                 self.lbd_name,
                                                 self.total_size - 1)
        uuid_bdev_2 = self.c.construct_lvol_bdev(uuid_store_2,
                                                 self.lbd_name,
                                                 self.total_size - 1)
        fail_count += self.c.check_get_bdevs_methods(uuid_bdev_1, self.total_size - 1)
        fail_count += self.c.check_get_bdevs_methods(uuid_bdev_2, self.total_size - 1)

        fail_count += self.c.delete_bdev(uuid_bdev_1)
        fail_count += self.c.delete_bdev(uuid_bdev_2)
        fail_count += self.c.destroy_lvol_store(uuid_store_1)
        fail_count += self.c.destroy_lvol_store(uuid_store_2)
        fail_count += self.c.delete_bdev(base_name_1)
        fail_count += self.c.delete_bdev(base_name_2)
        footer(53)
        return fail_count

    def test_case100(self):
        header(100)
        fail_count = 0
        if self.c.construct_lvol_bdev(self._gen_lvs_uudi(),
                                      self.lbd_name,
                                      32) == 0:
            fail_count += 1
        footer(100)
        return fail_count

    def test_case101(self):
        header(101)
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
        footer(101)
        return fail_count

    def test_case102(self):
        header(102)
        size = (self.total_size / 2) - 1
        base_name = self.c.construct_malloc_bdev(self.total_size,
                                                 self.block_size)
        uuid_store = self.c.construct_lvol_store(base_name,
                                                 self.lvs_name,
                                                 self.cluster_size)
        fail_count = self.c.check_get_lvol_stores(base_name, uuid_store,
                                                  self.cluster_size)
        uuid_bdev = self.c.construct_lvol_bdev(uuid_store,
                                               self.lbd_name,
                                               size)
        fail_count += self.c.check_get_bdevs_methods(uuid_bdev,
                                                     size)
        if self.c.construct_lvol_bdev(uuid_store,
                                      self.lbd_name,
                                      size) == 0:
            fail_count += 1

        self.c.delete_bdev(uuid_bdev)
        self.c.destroy_lvol_store(uuid_store)
        self.c.delete_bdev(base_name)
        footer(102)
        return fail_count

    def test_case150(self):
        header(150)
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
        footer(150)
        return fail_count

    def test_case200(self):
        header(200)
        fail_count = 0
        if self.c.resize_lvol_bdev(self._gen_lvb_uudi(), 16) == 0:
            fail_count += 1
        footer(200)
        return fail_count

    def test_case201(self):
        header(201)
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
        footer(201)
        return fail_count

    def test_case250(self):
        header(250)
        base_name = self.c.construct_malloc_bdev(self.total_size,
                                                 self.block_size)
        uuid_store = self.c.construct_lvol_store(base_name,
                                                 self.lvs_name,
                                                 self.cluster_size)
        fail_count = self.c.check_get_lvol_stores(base_name, uuid_store,
                                                  self.cluster_size)
        self.c.destroy_lvol_store(uuid_store)
        if self.c.check_get_lvol_stores("", "", "") == 1:
            fail_count += 1
        self.c.delete_bdev(base_name)
        footer(250)
        return fail_count

    def test_case251(self):
        header(251)
        base_name = self.c.construct_malloc_bdev(self.total_size,
                                                 self.block_size)
        uuid_store = self.c.construct_lvol_store(base_name,
                                                 self.lvs_name,
                                                 self.cluster_size)
        fail_count = self.c.check_get_lvol_stores(base_name, uuid_store,
                                                  self.cluster_size)
        fail_count += self.c.destroy_lvol_store(self.lvs_name)
        if self.c.check_get_lvol_stores("", "", "") == 1:
            fail_count += 1
        fail_count += self.c.delete_bdev(base_name)
        footer(251)
        return fail_count

    def test_case252(self):
        header(252)
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

        if self.c.check_get_lvol_stores("", "", "") == 1:
            fail_count += 1
        self.c.delete_bdev(base_name)
        footer(252)
        return fail_count

    def test_case253(self):
        header(253)
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
        if self.c.check_get_lvol_stores("", "", "") == 1:
            fail_count += 1
        self.c.delete_bdev(base_name)
        footer(253)
        return fail_count

    def test_case254(self):
        header(254)
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
        if self.c.check_get_lvol_stores("", "", "") == 1:
            fail_count += 1
        self.c.delete_bdev(base_name)
        footer(254)
        return fail_count

    def test_case255(self):
        header(255)
        base_path = path.dirname(sys.argv[0])
        base_name = "Nvme0n1"
        uuid_store = self.c.construct_lvol_store(base_name,
                                                 self.lvs_name,
                                                 self.cluster_size)
        fail_count = self.c.check_get_lvol_stores(base_name, uuid_store,
                                                  self.cluster_size)
        if self.c.destroy_lvol_store(self.lvs_name) != 0:
            fail_count += 1
        traddr = self._find_traddress_for_nvme("Nvme0")
        if traddr != -1:
            self.c.delete_bdev(base_name)
            self.c.construct_nvme_bdev("Nvme0", "PCIe", traddr)
            # wait 1 second to allow time for lvolstore tasting
            sleep(1)
        else:
            fail_count += 1
        ret_value = self.c.check_get_lvol_stores(base_name, uuid_store,
                                                 self.cluster_size)
        if ret_value == 0:
            fail_count += 1
        footer(255)
        return fail_count

    def test_case300(self):
        header(300)
        fail_count = 0
        if self.c.destroy_lvol_store(self._gen_lvs_uudi()) == 0:
            fail_count += 1
        footer(300)
        return fail_count

    def test_case301(self):
        header(301)
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

        footer(301)
        return fail_count

    def test_case350(self):
        print("Test of this feature not yet implemented.")
        pass
        return 0

    def test_case400(self):
        print("Test of this feature not yet implemented.")
        pass
        return 0

    # negative tests
    def test_case450(self):
        header(450)
        fail_count = 0
        bad_bdev_id = random.randrange(999999999)
        if self.c.construct_lvol_store(bad_bdev_id,
                                       self.lvs_name,
                                       self.cluster_size) == 0:
            fail_count += 1
        footer(450)
        return fail_count

    def test_case451(self):
        header(451)
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
        footer(451)
        return fail_count

    def test_case452(self):
        header(452)
        fail_count = 0
        base_name_1 = self.c.construct_malloc_bdev(self.total_size,
                                                   self.block_size)
        base_name_2 = self.c.construct_malloc_bdev(self.total_size,
                                                   self.block_size)
        uuid_store_1 = self.c.construct_lvol_store(base_name_1,
                                                   self.lvs_name,
                                                   self.cluster_size)
        fail_count += self.c.check_get_lvol_stores(base_name_1,
                                                   uuid_store_1,
                                                   self.cluster_size)
        if self.c.construct_lvol_store(base_name_2,
                                       self.lvs_name,
                                       self.cluster_size) == 0:
            fail_count += 1

        fail_count += self.c.destroy_lvol_store(uuid_store_1)
        fail_count += self.c.delete_bdev(base_name_1)
        fail_count += self.c.delete_bdev(base_name_2)

        footer(452)
        return fail_count

    def test_case500(self):
        print("Test of this feature not yet implemented.")
        pass
        return 0

    def test_case550(self):
        header(550)
        base_name = self.c.construct_malloc_bdev(self.total_size,
                                                 self.block_size)
        uuid_store = self.c.construct_lvol_store(base_name,
                                                 self.lvs_name,
                                                 self.cluster_size)
        fail_count = self.c.check_get_lvol_stores(base_name, uuid_store,
                                                  self.cluster_size)
        self.c.delete_bdev(base_name)
        if self.c.check_get_lvol_stores("", "", "") == 1:
            fail_count += 1
        footer(550)
        return fail_count

    def test_case600(self):
        header(600)
        fail_count = 0
        base_name = self.c.construct_malloc_bdev(self.total_size,
                                                 self.block_size)
        lvol_uuid = self.c.construct_lvol_store(base_name,
                                                self.lvs_name,
                                                (self.total_size * 1024 * 1024) + 1) == 0
        if self.c.check_get_lvol_stores(base_name, lvol_uuid) == 0:
            fail_count += 1
        fail_count += self.c.delete_bdev(base_name)
        footer(600)
        return fail_count

    def test_case601(self):
        header(601)
        fail_count = 0
        base_name = self.c.construct_malloc_bdev(self.total_size,
                                                 self.block_size)
        lvol_uuid = self.c.construct_lvol_store(base_name, self.lvs_name, 8191)
        if self.c.check_get_lvol_stores(base_name, lvol_uuid) == 0:
            fail_count += 1
        fail_count += self.c.delete_bdev(base_name)
        footer(601)
        return fail_count

    def test_case650(self):
        header(650)
        base_name = self.c.construct_malloc_bdev(self.total_size,
                                                 self.block_size)
        uuid_store = self.c.construct_lvol_store(base_name,
                                                 self.lvs_name,
                                                 self.cluster_size)
        fail_count = self.c.check_get_lvol_stores(base_name, uuid_store,
                                                  self.cluster_size)
        lvs = self.c.get_lvol_stores(self.lvs_name)[0]
        free_clusters_start = int(lvs['free_clusters'])
        bdev_size = int(lvs['cluster_size']) * int(lvs['free_clusters']) / MEGABYTE
        bdev_name = self.c.construct_lvol_bdev(uuid_store, self.lbd_name,
                                               bdev_size, thin=True)
        lvs = self.c.get_lvol_stores(self.lvs_name)[0]
        free_clusters_create_lvol = int(lvs[u'free_clusters'])
        if free_clusters_start != free_clusters_create_lvol:
            fail_count += 1
        lvol_bdev = self.c.get_lvol_bdev_with_name(bdev_name)
        nbd_name = "/dev/nbd0"
        fail_count += self.c.start_nbd_disk(bdev_name, nbd_name)

        size = int(lvs['cluster_size'])
        fail_count += self.run_fio_test("/dev/nbd0", 0, size, "write", "0xcc")
        lvs = self.c.get_lvol_stores(self.lvs_name)[0]
        free_clusters_first_fio = int(lvs[u'free_clusters'])
        if free_clusters_start != free_clusters_first_fio + 1:
            fail_count += 1

        size = int(lvs['cluster_size'])
        # calculate size of one and half cluster
        offset = int((int(lvol_bdev['num_blocks']) * int(lvol_bdev['block_size']) /
                      free_clusters_create_lvol) * 1.5)
        fail_count += self.run_fio_test(nbd_name, offset, size, "write", "0xcc")
        lvs = self.c.get_lvol_stores(self.lvs_name)[0]
        free_clusters_second_fio = int(lvs[u'free_clusters'])
        if free_clusters_start != free_clusters_second_fio + 3:
            fail_count += 1

        size = (free_clusters_create_lvol - 3) * int(lvs['cluster_size'])
        offset = int(int(lvol_bdev['num_blocks']) * int(lvol_bdev['block_size']) /
                     free_clusters_create_lvol * 3)
        fail_count += self.run_fio_test(nbd_name, offset, size, "write", "0xcc")
        lvs = self.c.get_lvol_stores(self.lvs_name)[0]
        free_clusters_third_fio = int(lvs[u'free_clusters'])
        if free_clusters_third_fio != 0:
            fail_count += 1

        fail_count += self.c.stop_nbd_disk(nbd_name)
        fail_count += self.c.delete_bdev(lvol_bdev['name'])
        lvs = self.c.get_lvol_stores(self.lvs_name)[0]
        free_clusters_end = int(lvs[u'free_clusters'])
        if free_clusters_start != free_clusters_end:
            fail_count += 1
        fail_count += self.c.destroy_lvol_store(uuid_store)
        footer(650)
        return fail_count

    def test_case651(self):
        header(651)
        base_name = self.c.construct_malloc_bdev(self.total_size,
                                                 self.block_size)
        uuid_store = self.c.construct_lvol_store(base_name,
                                                 self.lvs_name,
                                                 self.cluster_size)
        fail_count = self.c.check_get_lvol_stores(base_name, uuid_store,
                                                  self.cluster_size)
        lvs = self.c.get_lvol_stores(self.lvs_name)[0]
        free_clusters_start = int(lvs['free_clusters'])
        lbd_name0 = self.lbd_name + str("0")
        lbd_name1 = self.lbd_name + str("1")
        # calculate bdev size in megabytes
        bdev_size = int(lvs['cluster_size']) * int(lvs['free_clusters']) / MEGABYTE
        bdev_name0 = self.c.construct_lvol_bdev(uuid_store, lbd_name0,
                                                bdev_size, thin=False)
        bdev_name1 = self.c.construct_lvol_bdev(uuid_store, lbd_name1,
                                                bdev_size, thin=True)
        lvol_bdev0 = self.c.get_lvol_bdev_with_name(bdev_name0)
        lvol_bdev1 = self.c.get_lvol_bdev_with_name(bdev_name1)
        nbd_name0 = "/dev/nbd0"
        fail_count += self.c.start_nbd_disk(lvol_bdev0['name'], nbd_name0)
        nbd_name1 = "/dev/nbd1"
        fail_count += self.c.start_nbd_disk(lvol_bdev1['name'], nbd_name1)

        size = bdev_size * MEGABYTE
        fail_count += self.run_fio_test(nbd_name0, 0, size, "write", False)

        size = bdev_size * MEGABYTE
        fail_count += self.run_fio_test(nbd_name1, 0, size, "read", "0x00")

        fail_count += self.c.stop_nbd_disk(nbd_name0)
        fail_count += self.c.stop_nbd_disk(nbd_name1)
        fail_count += self.c.delete_bdev(lvol_bdev0['name'])
        fail_count += self.c.delete_bdev(lvol_bdev1['name'])
        fail_count += self.c.destroy_lvol_store(uuid_store)
        footer(651)
        return fail_count

    def test_case652(self):
        header(652)
        base_name = self.c.construct_malloc_bdev(self.total_size,
                                                 self.block_size)
        uuid_store = self.c.construct_lvol_store(base_name,
                                                 self.lvs_name,
                                                 self.cluster_size)
        fail_count = self.c.check_get_lvol_stores(base_name, uuid_store,
                                                  self.cluster_size)
        lvs = self.c.get_lvol_stores(self.lvs_name)[0]
        free_clusters_start = int(lvs['free_clusters'])
        bdev_size = int(lvs['cluster_size']) * int(lvs['free_clusters']) / MEGABYTE
        bdev_name = self.c.construct_lvol_bdev(uuid_store, self.lbd_name,
                                               bdev_size, thin=True)

        lvol_bdev = self.c.get_lvol_bdev_with_name(bdev_name)
        nbd_name = "/dev/nbd0"
        fail_count += self.c.start_nbd_disk(lvol_bdev['name'], nbd_name)
        size = bdev_size * MEGABYTE
        fail_count += self.run_fio_test(nbd_name, 0, size, "write", "0xcc")

        fail_count += self.c.stop_nbd_disk(nbd_name)
        fail_count += self.c.delete_bdev(lvol_bdev['name'])
        fail_count += self.c.destroy_lvol_store(uuid_store)
        footer(652)
        return fail_count

    def test_case653(self):
        header(653)
        # TODO
        fail_count = 0
        footer(653)
        return fail_count

    def test_case654(self):
        header(654)
        base_name = self.c.construct_malloc_bdev(self.total_size,
                                                 self.block_size)
        uuid_store = self.c.construct_lvol_store(base_name, self.lvs_name,
                                                 self.cluster_size)
        fail_count = self.c.check_get_lvol_stores(base_name, uuid_store,
                                                  self.cluster_size)
        lvs = self.c.get_lvol_stores(self.lvs_name)[0]
        free_clusters_start = int(lvs['free_clusters'])
        lbd_name0 = self.lbd_name + str("0")
        lbd_name1 = self.lbd_name + str("1")
        bdev_size = int(lvs['cluster_size']) * int(lvs['free_clusters']) / MEGABYTE
        bdev_name0 = self.c.construct_lvol_bdev(uuid_store, lbd_name0,
                                                bdev_size, thin=True)
        bdev_name1 = self.c.construct_lvol_bdev(uuid_store, lbd_name1,
                                                bdev_size, thin=True)

        lvs = self.c.get_lvol_stores(self.lvs_name)[0]
        free_clusters_create_lvol = int(lvs[u'free_clusters'])
        if free_clusters_start != free_clusters_create_lvol:
            fail_count += 1
        lvol_bdev0 = self.c.get_lvol_bdev_with_name(bdev_name0)
        lvol_bdev1 = self.c.get_lvol_bdev_with_name(bdev_name1)

        nbd_name0 = "/dev/nbd0"
        nbd_name1 = "/dev/nbd1"
        fail_count += self.c.start_nbd_disk(lvol_bdev0['name'], nbd_name0)
        fail_count += self.c.start_nbd_disk(lvol_bdev1['name'], nbd_name1)

        size = "75%"
        fail_count += self.run_fio_test(nbd_name0, 0, size, "write", "0xcc")

        size = "75%"
        fail_count += self.run_fio_test(nbd_name1, 0, size, "write", "0xee",
                                        expected_ret_value=1)

        size = "75%"
        fail_count += self.run_fio_test(nbd_name0, 0, size, "read", "0xcc")

        size = "25%"
        offset = "75%"
        fail_count += self.run_fio_test(nbd_name0, offset, size, "read", "0x00")

        fail_count += self.c.stop_nbd_disk(nbd_name0)
        fail_count += self.c.stop_nbd_disk(nbd_name1)
        fail_count += self.c.delete_bdev(lvol_bdev0['name'])
        fail_count += self.c.delete_bdev(lvol_bdev1['name'])
        fail_count += self.c.destroy_lvol_store(uuid_store)
        footer(654)
        return fail_count

    def test_case655(self):
        header(655)
        base_name = self.c.construct_malloc_bdev(self.total_size,
                                                 self.block_size)
        uuid_store = self.c.construct_lvol_store(base_name, self.lvs_name,
                                                 self.cluster_size)
        fail_count = self.c.check_get_lvol_stores(base_name, uuid_store,
                                                  self.cluster_size)
        lvs = self.c.get_lvol_stores(self.lvs_name)[0]
        free_clusters_start = int(lvs['free_clusters'])
        lbd_name0 = self.lbd_name + str("0")
        lbd_name1 = self.lbd_name + str("1")
        lvs_size = int(lvs['cluster_size']) * int(lvs['free_clusters']) / MEGABYTE
        bdev_size = int(lvs_size * 0.7)
        bdev_name0 = self.c.construct_lvol_bdev(uuid_store, lbd_name0,
                                                bdev_size, thin=True)
        bdev_name1 = self.c.construct_lvol_bdev(uuid_store, lbd_name1,
                                                bdev_size, thin=True)

        lvol_bdev0 = self.c.get_lvol_bdev_with_name(bdev_name0)
        lvol_bdev1 = self.c.get_lvol_bdev_with_name(bdev_name1)

        nbd_name0 = "/dev/nbd0"
        nbd_name1 = "/dev/nbd1"
        fail_count += self.c.start_nbd_disk(lvol_bdev0['name'], nbd_name0)
        fail_count += self.c.start_nbd_disk(lvol_bdev1['name'], nbd_name1)
        size = int(int(lvol_bdev0['num_blocks']) * int(lvol_bdev0['block_size']) * 0.7)
        fail_count += self.run_fio_test(nbd_name0, 0, size, "write", "0xcc")
        size = int(int(lvol_bdev1['num_blocks']) * int(lvol_bdev1['block_size']) * 0.7)
        fail_count += self.run_fio_test(nbd_name1, 0, size, "write", "0xee")

        fail_count += self.c.stop_nbd_disk(nbd_name0)
        fail_count += self.c.stop_nbd_disk(nbd_name1)
        fail_count += self.c.delete_bdev(lvol_bdev0['name'])
        fail_count += self.c.delete_bdev(lvol_bdev1['name'])
        fail_count += self.c.destroy_lvol_store(uuid_store)
        footer(655)
        return fail_count

    def test_case700(self):
        header(700)
        fail_count = 0
        uuid_bdevs = []
        base_name = "Nvme0n1"

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

        # size = approx 2% of total NVMe disk size
        _ = self.c.get_lvol_stores()[0]
        size = int(_["free_clusters"] / 50)

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
        if self._start_vhost(vhost_path, config_path, pid_path) != 0:
            fail_count += 1
            footer(700)
            return fail_count

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
            print("ERROR: old and loaded lvol bdev list count is not equal")

        for o, n in zip(old_bdevs, new_bdevs):
            if o != n:
                fail_count += 1
                print("ERROR: old and loaded lvol bdev is not the same")
                print("DIFF:")
                pprint.pprint([o, n])

        if fail_count != 0:
            footer(700)
            return fail_count

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

        footer(700)
        return fail_count

    def test_case701(self):
        header(701)
        base_name = "Nvme0n1"
        uuid_store = self.c.construct_lvol_store(base_name,
                                                 self.lvs_name,
                                                 self.cluster_size)
        fail_count = self.c.check_get_lvol_stores(base_name, uuid_store,
                                                  self.cluster_size)
        traddr = self._find_traddress_for_nvme("Nvme0")
        if traddr != -1:
            self.c.delete_bdev(base_name)
            self.c.construct_nvme_bdev("Nvme0", "PCIe", traddr)
            # wait 1 second to allow time for lvolstore tasting
            sleep(1)
        else:
            fail_count += 1
        if self.c.check_get_lvol_stores(base_name, uuid_store,
                                        self.cluster_size) != 0:
            fail_count += 1
        if self.c.destroy_lvol_store(uuid_store) != 0:
            fail_count += 1
        footer(701)
        return fail_count

    def test_case750(self):
        header(750)
        fail_count = 0
        nbd_name0 = "/dev/nbd0"
        nbd_name1 = "/dev/nbd1"
        snapshot_name = "snapshot0"
        base_name = self.c.construct_malloc_bdev(self.total_size,
                                                 self.block_size)
        uuid_store = self.c.construct_lvol_store(base_name,
                                                 self.lvs_name,
                                                 self.cluster_size)
        fail_count = self.c.check_get_lvol_stores(base_name, uuid_store,
                                                  self.cluster_size)

        lvs = self.c.get_lvol_stores()[0]
        free_clusters_start = int(lvs['free_clusters'])
        bdev_size = int(lvs['cluster_size']) * int(lvs['free_clusters']) / MEGABYTE / 3
        bdev_name = self.c.construct_lvol_bdev(uuid_store, self.lbd_name,
                                               bdev_size)
        lvol_bdev = self.c.get_lvol_bdev_with_name(bdev_name)
        fail_count += self.c.start_nbd_disk(lvol_bdev['name'], nbd_name0)
        size = bdev_size * MEGABYTE
        fail_count += self.run_fio_test(nbd_name0, 0, size, "write", "0xcc", 0)
        fail_count += self.c.snapshot_lvol_bdev(lvol_bdev['name'], snapshot_name)
        snapshot_bdev = self.c.get_lvol_bdev_with_name(self.lvs_name + "/" + snapshot_name)

        fail_count += self.c.start_nbd_disk(snapshot_bdev['name'], nbd_name1)
        size = bdev_size * MEGABYTE
        fail_count += self.run_fio_test(nbd_name1, 0, size, "write", "0xcc", 1)

        fail_count += self.c.stop_nbd_disk(nbd_name0)
        fail_count += self.c.stop_nbd_disk(nbd_name1)
        fail_count += self.c.delete_bdev(lvol_bdev['name'])
        fail_count += self.c.delete_bdev(snapshot_bdev['name'])
        fail_count += self.c.destroy_lvol_store(uuid_store)
        fail_count += self.c.delete_bdev(base_name)
        footer(750)
        return fail_count

    def test_case751(self):
        header(751)
        fail_count = 0
        nbd_name = ["/dev/nbd0", "/dev/nbd1", "/dev/nbd2", "/dev/nbd3"]
        snapshot_name0 = "snapshot0"
        snapshot_name1 = "snapshot1"
        base_name = self.c.construct_malloc_bdev(self.total_size,
                                                 self.block_size)
        uuid_store = self.c.construct_lvol_store(base_name,
                                                 self.lvs_name,
                                                 self.cluster_size)
        fail_count = self.c.check_get_lvol_stores(base_name, uuid_store,
                                                  self.cluster_size)
        lvs = self.c.get_lvol_stores()
        size = int(int(lvs[0][u'free_clusters'] * lvs[0]['cluster_size']) / 6 / MEGABYTE)
        lbd_name0 = self.lbd_name + str(0)
        lbd_name1 = self.lbd_name + str(1)
        uuid_bdev0 = self.c.construct_lvol_bdev(uuid_store,
                                                lbd_name0, size, thin=True)
        uuid_bdev1 = self.c.construct_lvol_bdev(uuid_store,
                                                lbd_name1, size, thin=False)
        lvol_bdev = self.c.get_lvol_bdev_with_name(uuid_bdev0)
        fail_count += self.c.start_nbd_disk(lvol_bdev['name'], nbd_name[0])
        fill_size = int(size * MEGABYTE / 2)
        fail_count = self.run_fio_test(nbd_name0, 0, fill_size, "write", "0xcc", 0)

        lvol_bdev = self.c.get_lvol_bdev_with_name(uuid_bdev1)
        fail_count += self.c.start_nbd_disk(lvol_bdev['name'], nbd_name[1])
        fill_size = size * MEGABYTE
        fail_count = self.run_fio_test(nbd_name1, 0, fill_size, "write", "0xcc", 0)

        fail_count += self.c.snapshot_lvol_bdev(uuid_bdev0, snapshot_name0)
        fail_count += self.c.snapshot_lvol_bdev(uuid_bdev1, snapshot_name1)
        fail_count += self.c.start_nbd_disk(self.lvs_name + "/" + snapshot_name0, nbd_name[2])
        fail_count += self.c.start_nbd_disk(self.lvs_name + "/" + snapshot_name1, nbd_name[3])
        fail_count += self.compare_two_disks(nbd_name[0], nbd_name[2], 0)
        fail_count += self.compare_two_disks(nbd_name[1], nbd_name[3], 0)

        fill_size = int(size * MEGABYTE / 2)
        offset = fill_size
        fail_count += self.run_fio_test(nbd_name[0], offset, fill_size, "write", "0xcc", 0)
        fail_count += self.compare_two_disks(nbd_name[0], nbd_name[2], 1)
        for nbd in nbd_name:
            fail_count += self.c.stop_nbd_disk(nbd)
        fail_count += self.c.delete_bdev(lvol_bdev0['name'])
        fail_count += self.c.delete_bdev(lvol_bdev1['name'])
        fail_count += self.c.delete_bdev(self.lvs_name + "/" + snapshot_name0)
        fail_count += self.c.delete_bdev(self.lvs_name + "/" + snapshot_name1)
        fail_count += self.c.destroy_lvol_store(uuid_store)
        fail_count += self.c.delete_bdev(base_name)
        footer(751)
        return fail_count

    def test_case752(self):
        header(752)
        fail_count = 0
        nbd_name = "/dev/nbd0"
        snapshot_name = "snapshot"
        base_name = self.c.construct_malloc_bdev(self.total_size,
                                                 self.block_size)
        uuid_store = self.c.construct_lvol_store(base_name,
                                                 self.lvs_name,
                                                 self.cluster_size)
        fail_count = self.c.check_get_lvol_stores(base_name, uuid_store,
                                                  self.cluster_size)
        lvs = self.c.get_lvol_stores()
        size = int(int(lvs[0][u'free_clusters'] * lvs[0]['cluster_size']) / 2 / MEGABYTE)
        uuid_bdev = self.c.construct_lvol_bdev(uuid_store, self.lbd_name,
                                               size, thin=True)

        lvol_bdev = self.c.get_lvol_bdev_with_name(uuid_bdev)
        fail_count += self.c.start_nbd_disk(lvol_bdev['name'], nbd_name)
        fill_size = int(size * MEGABYTE)
        thread = FioThread(nbd_name, 0, fill_size, "write", "0xcc", 0)
        thread.start()
        time.sleep(2)
        fail_count += self.c.snapshot_lvol_bdev(lvol_bdev['name'], snapshot_name)
        thread.join()
        fail_count += thread.rv
        fail_count += self.c.stop_nbd_disk(nbd_name)
        fail_count += self.c.delete_bdev(lvol_bdev['name'])
        fail_count += self.c.delete_bdev(self.lvs_name + "/" + snapshot_name)
        fail_count += self.c.destroy_lvol_store(uuid_store)
        fail_count += self.c.delete_bdev(base_name)
        footer(752)
        return fail_count

    def test_case753(self):
        header(753)
        fail_count = 0
        snapshot_name0 = "snapshot0"
        snapshot_name1 = "snapshot1"
        base_name = self.c.construct_malloc_bdev(self.total_size,
                                                 self.block_size)
        uuid_store = self.c.construct_lvol_store(base_name,
                                                 self.lvs_name,
                                                 self.cluster_size)
        fail_count = self.c.check_get_lvol_stores(base_name, uuid_store,
                                                  self.cluster_size)
        lvs = self.c.get_lvol_stores()
        size = int(int(lvs[0][u'free_clusters'] * lvs[0]['cluster_size']) / 2 / MEGABYTE)
        uuid_bdev = self.c.construct_lvol_bdev(uuid_store, self.lbd_name,
                                               size, thin=True)

        lvol_bdev = self.c.get_lvol_bdev_with_name(uuid_bdev)
        fail_count += self.c.snapshot_lvol_bdev(lvol_bdev['name'], snapshot_name0)
        if self.c.snapshot_lvol_bdev(snapshot_name0, snapshot_name1) == 0:
            print("ERROR: Creating snapshot of snapshot should fail")
            fail_count += 1
        fail_count += self.c.delete_bdev(lvol_bdev['name'])
        fail_count += self.c.delete_bdev(self.lvs_name + "/" + snapshot_name0)
        fail_count += self.c.destroy_lvol_store(uuid_store)
        fail_count += self.c.delete_bdev(base_name)
        footer(753)
        return fail_count

    def test_case754(self):
        header(754)
        fail_count = 0
        clone_name = "clone"
        base_name = self.c.construct_malloc_bdev(self.total_size,
                                                 self.block_size)
        uuid_store = self.c.construct_lvol_store(base_name,
                                                 self.lvs_name,
                                                 self.cluster_size)
        fail_count = self.c.check_get_lvol_stores(base_name, uuid_store,
                                                  self.cluster_size)
        lvs = self.c.get_lvol_stores()
        size = int(int(lvs[0][u'free_clusters'] * lvs[0]['cluster_size']) / 2 / MEGABYTE)
        uuid_bdev = self.c.construct_lvol_bdev(uuid_store, self.lbd_name,
                                               size, thin=True)

        lvol_bdev = self.c.get_lvol_bdev_with_name(uuid_bdev)

        rv = self.c.clone_lvol_bdev(lvol_bdev['name'], clone_name)
        if rv == 0:
            print("ERROR: Creating clone of lvol bdev ended with unexpected success")
            fail_count += 1
        snapshot_name = "snapshot"
        fail_count += self.c.snapshot_lvol_bdev(lvol_bdev['name'], snapshot_name)
        rv = self.c.clone_lvol_bdev(lvol_bdev['name'], clone_name)
        if rv == 0:
            print("ERROR: Creating clone of lvol bdev ended with unexpected success")
            fail_count += 1
        rv = self.c.clone_lvol_bdev(snapshot_name, clone_name)
        if rv != 0:
            print("ERROR: Creating clone of snapshot ended with unexpected failure")
            fail_count += 1
        clone_bdev = self.c.get_lvol_bdev_with_name(clone_name)

        fail_count += self.c.delete_bdev(lvol_bdev['name'])
        fail_count += self.c.delete_bdev(clone_bdev['name'])
        fail_count += self.c.delete_bdev(self.lvs_name + "/" + snapshot_name)
        fail_count += self.c.destroy_lvol_store(uuid_store)
        fail_count += self.c.delete_bdev(base_name)
        footer(754)
        return fail_count

    def test_case755(self):
        header(755)
        fail_count = 0
        nbd_name = ["/dev/nbd0", "/dev/nbd1", "/dev/nbd2", "/dev/nbd3"]
        snapshot_name = "snapshot"
        clone_name0 = "clone0"
        clone_name1 = "clone1"
        base_name = self.c.construct_malloc_bdev(self.total_size,
                                                 self.block_size)
        uuid_store = self.c.construct_lvol_store(base_name,
                                                 self.lvs_name,
                                                 self.cluster_size)
        fail_count = self.c.check_get_lvol_stores(base_name, uuid_store,
                                                  self.cluster_size)
        lvs = self.c.get_lvol_stores()
        size = int(int(lvs[0][u'free_clusters'] * lvs[0]['cluster_size']) / 6 / MEGABYTE)
        lbd_name0 = self.lbd_name + str(0)
        uuid_bdev0 = self.c.construct_lvol_bdev(uuid_store,
                                                lbd_name0, size, thin=True)
        lvol_bdev = self.c.get_lvol_bdev_with_name(uuid_bdev0)
        fail_count += self.c.start_nbd_disk(lvol_bdev['name'], nbd_name[0])
        fill_size = size * MEGABYTE
        fail_count = self.run_fio_test(nbd_name0, 0, fill_size, "write", "0xcc", 0)

        fail_count += self.c.snapshot_lvol_bdev(lvol_bdev['name'], snapshot_name)
        snapshot_bdev = self.c.get_lvol_bdev_with_name(self.lvs_name + "/" + snapshot_name)
        fail_count = self.c.clone_lvol_bdev(self, snapshot_bdev['name'], clone_name0)
        fail_count = self.c.clone_lvol_bdev(self, snapshot_bdev['name'], clone_name1)

        lvol_clone = self.c.get_lvol_bdev_with_name(clone_name0)
        fail_count += self.c.start_nbd_disk(lvol_clone['name'], nbd_name[1])
        fill_size = int(size * MEGABYTE / 2)
        fail_count += self.run_fio_test(nbd_name[1], 0, fill_size, "write", "0xcc", 0)
        fail_count += self.c.start_nbd_disk(self.lvs_name + "/" + snapshot_name, nbd_name[2])
        lvol_clone1 = self.c.get_lvol_bdev_with_name(clone_name1)
        fail_count += self.c.start_nbd_disk(lvol_clone1['name'], nbd_name[3])
        fail_count += self.compare_two_disks(nbd_name[2], nbd_name[3], 0)

        fail_count += self.c.delete_bdev(lvol_bdev['name'])
        fail_count += self.c.delete_bdev(snapshot_bdev['name'])
        fail_count += self.c.delete_bdev(clone_name0)
        fail_count += self.c.delete_bdev(clone_name1)
        fail_count += self.c.destroy_lvol_store(uuid_store)
        fail_count += self.c.delete_bdev(base_name)
        footer(755)
        return fail_count

    def test_case10000(self):
        header(10000)
        pid_path = path.join(self.path, 'vhost.pid')

        base_name = self.c.construct_malloc_bdev(self.total_size,
                                                 self.block_size)
        uuid_store = self.c.construct_lvol_store(base_name,
                                                 self.lvs_name,
                                                 self.cluster_size)
        fail_count = self.c.check_get_lvol_stores(base_name, uuid_store,
                                                  self.cluster_size)

        fail_count += self._stop_vhost(pid_path)
        footer(10000)
        return fail_count
