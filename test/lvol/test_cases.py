import io
import math
import time
import sys
import random
import signal
import subprocess
import pprint
import socket
import threading
import os

from errno import ESRCH
from os import kill, path, unlink, path, listdir, remove
from rpc_commands_lib import Commands_Rpc
from time import sleep
from uuid import uuid4


MEGABYTE = 1024 * 1024


current_fio_pid = -1

# ## Objective
# The purpose of these tests is to verify the possibility of using lvol configuration in SPDK.
#
# ## Methodology
# Configuration in test is to be done using example stub application.
# All management is done using RPC calls, including logical volumes management.
# All tests are performed using malloc backends.
# One exception to malloc backends are tests for logical volume
# tasting - these require persistent merory like NVMe backend.
#
# Tests will be executed as scenarios - sets of smaller test step
# in which return codes from RPC calls is validated.
# Some configuration calls may also be validated by use of
# "get_*" RPC calls, which provide additional information for verifying
# results.
#
# Tests with thin provisioned lvol bdevs, snapshots and clones are using nbd devices.
# Before writing/reading to lvol bdev, bdev is installed with rpc nbd_start_disk.
# After finishing writing/reading, rpc nbd_stop_disk is used.


def is_process_alive(pid):
    try:
        os.kill(pid, 0)
    except Exception as e:
        return 1

    return 0


def get_fio_cmd(nbd_disk, offset, size, rw, pattern, extra_params=""):
    fio_template = "fio --name=fio_test --filename=%(file)s --offset=%(offset)s --size=%(size)s"\
                   " --rw=%(rw)s --direct=1 %(extra_params)s %(pattern)s"
    pattern_template = ""
    if pattern:
        pattern_template = "--do_verify=1 --verify=pattern --verify_pattern=%s"\
                           " --verify_state_save=0" % pattern
    fio_cmd = fio_template % {"file": nbd_disk, "offset": offset, "size": size,
                              "rw": rw, "pattern": pattern_template,
                              "extra_params": extra_params}

    return fio_cmd


def run_fio(fio_cmd, expected_ret_value):
    global current_fio_pid
    try:
        proc = subprocess.Popen([fio_cmd], shell=True)
        current_fio_pid = proc.pid
        proc.wait()
        rv = proc.returncode
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
    def __init__(self, nbd_disk, offset, size, rw, pattern, expected_ret_value,
                 extra_params=""):
        super(FioThread, self).__init__()
        self.fio_cmd = get_fio_cmd(nbd_disk, offset, size, rw, pattern,
                                   extra_params=extra_params)
        self.rv = 1
        self.expected_ret_value = expected_ret_value

    def run(self):
        print("INFO: Starting fio")
        self.rv = run_fio(self.fio_cmd, self.expected_ret_value)
        print("INFO: Fio test finished")


def test_counter():
    '''
    :return: the number of tests
    '''
    return ['test_case' in i for i in dir(TestCases)].count(True)


def case_message(func):
    def inner(*args, **kwargs):
        test_name = {
            # bdev_lvol_delete_lvstore - positive tests
            252: 'bdev_lvol_delete_lvstore_with_lvol_bdev_positive',
            253: 'destroy_multi_logical_volumes_positive',
            254: 'destroy_after_bdev_lvol_resize_positive',
            255: 'delete_lvol_store_persistent_positive',
            # bdev_lvol_delete_lvstore - negative tests
            300: 'bdev_lvol_delete_lvstore_nonexistent_lvs_uuid',
            301: 'delete_lvol_store_underlying_bdev',
            550: 'delete_bdev_positive',
            551: 'delete_lvol_bdev',
            552: 'bdev_lvol_delete_lvstore_with_clones',
            553: 'unregister_lvol_bdev',
            602: 'bdev_lvol_create_lvstore_with_all_clear_methods',
            # Provisioning
            650: 'thin_provisioning_check_space',
            651: 'thin_provisioning_read_empty_bdev',
            652: 'thin_provisioning_data_integrity_test',
            653: 'thin_provisioning_resize',
            654: 'thin_overprovisioning',
            655: 'thin_provisioning_filling_disks_less_than_lvs_size',
            # logical volume tasting tests
            700: 'tasting_positive',
            701: 'tasting_lvol_store_positive',
            702: 'tasting_positive_with_different_lvol_store_cluster_size',
            # snapshot and clone
            750: 'snapshot_readonly',
            751: 'snapshot_compare_with_lvol_bdev',
            752: 'snapshot_during_io_traffic',
            753: 'snapshot_of_snapshot',
            754: 'clone_bdev_only',
            755: 'clone_writing_clone',
            756: 'clone_and_snapshot_consistency',
            757: 'clone_inflate',
            758: 'decouple_parent',
            759: 'decouple_parent_rw',
            760: 'set_read_only',
            761: 'delete_snapshot',
            762: 'delete_snapshot_with_snapshot',
            # logical volume rename tests
            800: 'rename_positive',
            801: 'rename_lvs_nonexistent',
            802: 'rename_lvs_EEXIST',
            803: 'bdev_lvol_rename_nonexistent',
            804: 'bdev_lvol_rename_EEXIST',
            # logical volume clear_method test
            850: 'clear_method_none',
            851: 'clear_method_unmap',
            # SIGTERM
            10000: 'SIGTERM',
        }
        num = int(func.__name__.strip('test_case')[:])
        print("************************************")
        print("START TEST CASE {name}".format(name=test_name[num]))
        print("************************************")
        fail_count = func(*args, **kwargs)
        print("************************************")
        if not fail_count:
            print("END TEST CASE {name} PASS".format(name=test_name[num]))
        else:
            print("END TEST CASE {name} FAIL".format(name=test_name[num]))
        print("************************************")
        return fail_count
    return inner


class TestCases(object):

    def __init__(self, rpc_py, total_size, block_size, base_dir_path, app_path):
        self.c = Commands_Rpc(rpc_py)
        self.total_size = total_size
        self.block_size = block_size
        self.cluster_size = None
        self.path = base_dir_path
        self.app_path = app_path
        self.lvs_name = "lvs_test"
        self.lbd_name = "lbd_test"
        self.vhost_config_path = path.join(path.dirname(sys.argv[0]), 'vhost.conf')

    def _gen_lvs_uuid(self):
        return str(uuid4())

    def _gen_lvb_uuid(self):
        return "_".join([str(uuid4()), str(random.randrange(9999999999))])

    def compare_two_disks(self, disk1, disk2, expected_ret_value):
        cmp_cmd = "cmp %s %s" % (disk1, disk2)
        try:
            process = subprocess.check_output(cmp_cmd, stderr=subprocess.STDOUT, shell=True)
            rv = 0
        except subprocess.CalledProcessError as ex:
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
                except OSError as err:
                    if err.errno == ESRCH:
                        pass
                    else:
                        return 1
                else:
                    return 1
            else:
                return 1
        return 0

    def _start_vhost(self, vhost_path, pid_path):
        subprocess.call("{app} -f "
                        "{pid} &".format(app=vhost_path,
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

    def get_lvs_size(self, lvs_name="lvs_test"):
        lvs = self.c.bdev_lvol_get_lvstores(lvs_name)[0]
        return int(int(lvs['free_clusters'] * lvs['cluster_size']) / MEGABYTE)

    def get_lvs_divided_size(self, split_num, lvs_name="lvs_test"):
        # Actual size of lvol bdevs on creation is rounded up to multiple of cluster size.
        # In order to avoid over provisioning, this function returns
        # lvol store size in MB divided by split_num - rounded down to multiple of cluster size."
        lvs = self.c.bdev_lvol_get_lvstores(lvs_name)[0]
        return int(int(lvs['free_clusters'] / split_num) * lvs['cluster_size'] / MEGABYTE)

    def get_lvs_cluster_size(self, lvs_name="lvs_test"):
        lvs = self.c.bdev_lvol_get_lvstores(lvs_name)[0]
        return int(int(lvs['cluster_size']) / MEGABYTE)

    @case_message
    def test_case252(self):
        """
        bdev_lvol_delete_lvstore_with_lvol_bdev_positive

        Positive test for destroying a logical volume store with lvol bdev
        created on top.
        Call bdev_lvol_delete_lvstore with correct logical_volumes name
        """
        # Create malloc bdev
        base_name = self.c.bdev_malloc_create(self.total_size,
                                              self.block_size)
        # Construct lvol store on created malloc bdev
        uuid_store = self.c.bdev_lvol_create_lvstore(base_name,
                                                     self.lvs_name)
        # Check correct uuid values in response bdev_lvol_get_lvstores command
        fail_count = self.c.check_bdev_lvol_get_lvstores(base_name, uuid_store,
                                                         self.cluster_size)
        # Construct lvol bdev on correct lvs_uuid
        # and size is equal to size malloc bdev
        lvs_size = self.get_lvs_size()
        uuid_bdev = self.c.bdev_lvol_create(uuid_store,
                                            self.lbd_name,
                                            lvs_size)
        fail_count += self.c.check_bdev_get_bdevs_methods(uuid_bdev,
                                                          lvs_size)
        # Destroy lvol store
        if self.c.bdev_lvol_delete_lvstore(uuid_store) != 0:
            fail_count += 1

        # Check correct response bdev_lvol_get_lvstores command
        if self.c.check_bdev_lvol_get_lvstores("", "", "") == 1:
            fail_count += 1
        self.c.bdev_malloc_delete(base_name)

        # Expected result:
        # - calls successful, return code = 0
        # - bdev_lvol_get_lvstores: response should be of no value after destroyed lvol store
        # - no other operation fails
        return fail_count

    @case_message
    def test_case253(self):
        """
        Name: destroy_multi_logical_volumes_positive

        Positive test for destroying a logical volume store with multiple lvol
        bdevs created on top.
        Call bdev_lvol_create with correct lvol store UUID and
        size is equal to one quarter of the this bdev size.
        """
        # Create malloc bdev
        base_name = self.c.bdev_malloc_create(self.total_size,
                                              self.block_size)
        # Construct lvol store on correct, exisitng malloc bdev
        uuid_store = self.c.bdev_lvol_create_lvstore(base_name,
                                                     self.lvs_name)
        # Check correct uuid values in response bdev_lvol_get_lvstores command
        fail_count = self.c.check_bdev_lvol_get_lvstores(base_name, uuid_store,
                                                         self.cluster_size)
        size = self.get_lvs_divided_size(4)
        # Construct four lvol bdevs on correct lvs_uuid and
        # size is equal to one quarter of the lvol size
        for i in range(4):
            uuid_bdev = self.c.bdev_lvol_create(uuid_store,
                                                self.lbd_name + str(i),
                                                size)
            fail_count += self.c.check_bdev_get_bdevs_methods(uuid_bdev, size)

        # Destroy lvol store
        self.c.bdev_lvol_delete_lvstore(uuid_store)
        # Check correct response bdev_lvol_get_lvstores command
        if self.c.check_bdev_lvol_get_lvstores("", "", "") == 1:
            fail_count += 1
        self.c.bdev_malloc_delete(base_name)

        # Expected result:
        # - call successful, return code = 0
        # - get_lvol_store: backend used for bdev_lvol_create has name
        #   field set with the same name as returned from RPC call for all repeat
        # - no other operation fails
        return fail_count

    @case_message
    def test_case254(self):
        """
        destroy_resize_logical_volume_positive

        Positive test for destroying a logical_volume after resizing.
        Call bdev_lvol_delete_lvstore with correct logical_volumes name.
        """
        # Create malloc bdev
        base_name = self.c.bdev_malloc_create(self.total_size,
                                              self.block_size)
        # Construct lvol store on create malloc bdev
        uuid_store = self.c.bdev_lvol_create_lvstore(base_name,
                                                     self.lvs_name)
        # Check correct uuid values in response bdev_lvol_get_lvstores command
        fail_count = self.c.check_bdev_lvol_get_lvstores(base_name, uuid_store,
                                                         self.cluster_size)
        size = self.get_lvs_divided_size(4)
        # bdev_lvol_create on correct lvs_uuid and size is
        # equal to one quarter of size malloc bdev
        uuid_bdev = self.c.bdev_lvol_create(uuid_store,
                                            self.lbd_name,
                                            size)
        # check size of the lvol bdev
        fail_count += self.c.check_bdev_get_bdevs_methods(uuid_bdev, size)
        sz = size + 4
        # Resize_lvol_bdev on correct lvs_uuid and size is
        # equal to one quarter of size malloc bdev plus 4 MB
        self.c.bdev_lvol_resize(uuid_bdev, sz)
        # check size of the lvol bdev by command RPC : bdev_get_bdevs
        fail_count += self.c.check_bdev_get_bdevs_methods(uuid_bdev, sz)
        # Resize_lvol_bdev on correct lvs_uuid and size is
        # equal half of size malloc bdev
        sz = size * 2
        self.c.bdev_lvol_resize(uuid_bdev, sz)
        # check size of the lvol bdev by command RPC : bdev_get_bdevs
        fail_count += self.c.check_bdev_get_bdevs_methods(uuid_bdev, sz)
        # Resize_lvol_bdev on correct lvs_uuid and size is
        # equal to three quarters of size malloc bdev
        sz = size * 3
        self.c.bdev_lvol_resize(uuid_bdev, sz)
        # check size of the lvol bdev by command RPC : bdev_get_bdevs
        fail_count += self.c.check_bdev_get_bdevs_methods(uuid_bdev, sz)
        # Resize_lvol_bdev on correct lvs_uuid and size is
        # equal to size if malloc bdev minus 4 MB
        sz = (size * 4) - 4
        self.c.bdev_lvol_resize(uuid_bdev, sz)
        # check size of the lvol bdev by command RPC : bdev_get_bdevs
        fail_count += self.c.check_bdev_get_bdevs_methods(uuid_bdev, sz)
        # Resize_lvol_bdev on the correct lvs_uuid and size is equal 0 MiB
        sz = 0
        self.c.bdev_lvol_resize(uuid_bdev, sz)
        # check size of the lvol bdev by command RPC : bdev_get_bdevs
        fail_count += self.c.check_bdev_get_bdevs_methods(uuid_bdev, sz)

        # Destroy lvol store
        self.c.bdev_lvol_delete_lvstore(uuid_store)
        if self.c.check_bdev_lvol_get_lvstores("", "", "") == 1:
            fail_count += 1
        self.c.bdev_malloc_delete(base_name)

        # Expected result:
        # - lvol bdev should change size after resize operations
        # - calls successful, return code = 0
        # - no other operation fails
        # - bdev_lvol_get_lvstores: response should be of no value after destroyed lvol store
        return fail_count

    @case_message
    def test_case255(self):
        """
        delete_lvol_store_persistent_positive

        Positive test for removing lvol store persistently
        """
        base_path = path.dirname(sys.argv[0])
        base_name = "aio_bdev0"
        aio_bdev0 = path.join(base_path, "aio_bdev_0")
        # Construct aio bdev
        self.c.bdev_aio_create(aio_bdev0, base_name, 4096)
        # Create lvol store on created aio bdev
        uuid_store = self.c.bdev_lvol_create_lvstore(base_name,
                                                     self.lvs_name)
        fail_count = self.c.check_bdev_lvol_get_lvstores(base_name, uuid_store,
                                                         self.cluster_size)
        # Destroy lvol store
        if self.c.bdev_lvol_delete_lvstore(self.lvs_name) != 0:
            fail_count += 1

        # Delete aio bdev
        self.c.bdev_aio_delete(base_name)
        # Create aio bdev on the same file
        self.c.bdev_aio_create(aio_bdev0, base_name, 4096)
        # Wait 1 second to allow time for lvolstore tasting
        sleep(1)

        # check if destroyed lvol store does not exist on aio bdev
        ret_value = self.c.check_bdev_lvol_get_lvstores(base_name, uuid_store,
                                                        self.cluster_size)
        if ret_value == 0:
            fail_count += 1
        self.c.bdev_aio_delete(base_name)

        # Expected result:
        # - bdev_lvol_get_lvstores should not report any existsing lvol stores in configuration
        #    after deleting and adding NVMe bdev
        # - no other operation fails
        return fail_count

    @case_message
    def test_case300(self):
        """
        bdev_lvol_delete_lvstore_nonexistent_lvs_uuid

        Call bdev_lvol_delete_lvstore with nonexistent logical_volumes name
        exist in configuration.
        """
        fail_count = 0
        # try to call bdev_lvol_delete_lvstore with lvs_uuid which does not exist
        if self.c.bdev_lvol_delete_lvstore(self._gen_lvs_uuid()) == 0:
            fail_count += 1

        # Expected result:
        # - return code != 0
        # - Error code response printed to stdout
        return fail_count

    @case_message
    def test_case301(self):
        """
        delete_lvol_store_underlying_bdev

        Call bdev_lvol_delete_lvstore after deleting it's base bdev.
        Lvol store should be automatically removed on deleting underlying bdev.
        """
        # Create malloc bdev
        base_name = self.c.bdev_malloc_create(self.total_size,
                                              self.block_size)
        # Construct lvol store on created malloc bdev
        uuid_store = self.c.bdev_lvol_create_lvstore(base_name,
                                                     self.lvs_name)
        # Check correct uuid values in response bdev_lvol_get_lvstores command
        fail_count = self.c.check_bdev_lvol_get_lvstores(base_name, uuid_store,
                                                         self.cluster_size)

        # Delete malloc bdev
        if self.c.bdev_malloc_delete(base_name) != 0:
            fail_count += 1

        # Try to destroy lvol store. This call should fail as lvol store
        # is no longer present
        if self.c.bdev_lvol_delete_lvstore(uuid_store) == 0:
            fail_count += 1

        # Expected result:
        # - bdev_lvol_delete_lvstore return code != 0
        # - Error code: ENODEV ("No such device") response printed to stdout
        # - no other operation fails
        return fail_count

    # negative tests
    @case_message
    def test_case550(self):
        """
        delete_bdev_positive

        Positive test for deleting malloc bdev.
        Call bdev_lvol_create_lvstore with correct base bdev name.
        """
        # Create malloc bdev
        base_name = self.c.bdev_malloc_create(self.total_size,
                                              self.block_size)
        # Construct_lvol_store on correct, exisitng malloc bdev
        uuid_store = self.c.bdev_lvol_create_lvstore(base_name,
                                                     self.lvs_name)
        # Check correct uuid values in response bdev_lvol_get_lvstores command
        fail_count = self.c.check_bdev_lvol_get_lvstores(base_name, uuid_store,
                                                         self.cluster_size)
        # Delete malloc bdev
        self.c.bdev_malloc_delete(base_name)
        #  Check response bdev_lvol_get_lvstores command
        if self.c.check_bdev_lvol_get_lvstores("", "", "") == 1:
            fail_count += 1

        # Expected result:
        # - bdev_lvol_get_lvstores: response should be of no value after destroyed lvol store
        # - no other operation fails
        return fail_count

    @case_message
    def test_case551(self):
        """
        bdev_lvol_delete_ordering

        Test for destroying lvol bdevs in particular order.
        Check destroying wrong one is not possible and returns error.
        """

        fail_count = 0
        snapshot_name = "snapshot"
        clone_name = "clone"

        # Create malloc bdev
        base_name = self.c.bdev_malloc_create(self.total_size,
                                              self.block_size)
        # Construct_lvol_store on correct, exisitng malloc bdev
        uuid_store = self.c.bdev_lvol_create_lvstore(base_name,
                                                     self.lvs_name,
                                                     self.cluster_size)
        # Check correct uuid values in response bdev_lvol_get_lvstores command
        fail_count = self.c.check_bdev_lvol_get_lvstores(base_name, uuid_store,
                                                         self.cluster_size)
        lvs = self.c.bdev_lvol_get_lvstores()
        size = int(int(lvs[0]['free_clusters'] * lvs[0]['cluster_size']) / 4 / MEGABYTE)

        # Construct thin provisioned lvol bdev
        uuid_bdev0 = self.c.bdev_lvol_create(uuid_store,
                                             self.lbd_name, size, thin=True)
        lvol_bdev = self.c.get_lvol_bdev_with_name(uuid_bdev0)

        # Create snapshot of thin provisioned lvol bdev
        fail_count += self.c.bdev_lvol_snapshot(lvol_bdev['name'], snapshot_name)
        snapshot_bdev = self.c.get_lvol_bdev_with_name(self.lvs_name + "/" + snapshot_name)

        # Create clone of snapshot and check if it ends with success
        fail_count += self.c.bdev_lvol_clone(self.lvs_name + "/" + snapshot_name, clone_name)
        clone_bdev = self.c.get_lvol_bdev_with_name(self.lvs_name + "/" + clone_name)

        # Try to destroy snapshot with clones and check if it fails
        ret_value = self.c.bdev_lvol_delete(snapshot_bdev['name'])
        if ret_value == 0:
            print("ERROR: Delete snapshot should fail but didn't")
            fail_count += 1

        # Destroy clone and then snapshot
        fail_count += self.c.bdev_lvol_delete(lvol_bdev['name'])
        fail_count += self.c.bdev_lvol_delete(clone_bdev['name'])
        fail_count += self.c.bdev_lvol_delete(snapshot_bdev['name'])

        # Destroy lvol store
        fail_count += self.c.bdev_lvol_delete_lvstore(uuid_store)

        #  Check response bdev_lvol_get_lvstores command
        if self.c.check_bdev_lvol_get_lvstores("", "", "") == 1:
            fail_count += 1

        # Delete malloc bdev
        self.c.bdev_malloc_delete(base_name)
        # Expected result:
        # - bdev_lvol_get_lvstores: response should be of no value after destroyed lvol store
        # - no other operation fails
        return fail_count

    @case_message
    def test_case552(self):
        """
        bdev_lvol_delete_lvstore_with_clones

        Test for destroying lvol store with clones present,
        without removing them first.
        """

        fail_count = 0
        snapshot_name = "snapshot"
        snapshot_name2 = "snapshot2"
        clone_name = "clone"

        # Create malloc bdev
        base_name = self.c.bdev_malloc_create(self.total_size,
                                              self.block_size)
        # Construct_lvol_store on correct, exisitng malloc bdev
        uuid_store = self.c.bdev_lvol_create_lvstore(base_name,
                                                     self.lvs_name,
                                                     self.cluster_size)
        # Check correct uuid values in response bdev_lvol_get_lvstores command
        fail_count = self.c.check_bdev_lvol_get_lvstores(base_name, uuid_store,
                                                         self.cluster_size)
        lvs = self.c.bdev_lvol_get_lvstores()
        size = int(int(lvs[0]['free_clusters'] * lvs[0]['cluster_size']) / 4 / MEGABYTE)

        # Create lvol bdev, snapshot it, then clone it and then snapshot the clone
        uuid_bdev0 = self.c.bdev_lvol_create(uuid_store, self.lbd_name, size, thin=True)
        lvol_bdev = self.c.get_lvol_bdev_with_name(uuid_bdev0)

        fail_count += self.c.bdev_lvol_snapshot(lvol_bdev['name'], snapshot_name)
        snapshot_bdev = self.c.get_lvol_bdev_with_name(self.lvs_name + "/" + snapshot_name)

        fail_count += self.c.bdev_lvol_clone(self.lvs_name + "/" + snapshot_name, clone_name)
        clone_bdev = self.c.get_lvol_bdev_with_name(self.lvs_name + "/" + clone_name)

        fail_count += self.c.bdev_lvol_snapshot(clone_bdev['name'], snapshot_name2)
        snapshot_bdev2 = self.c.get_lvol_bdev_with_name(self.lvs_name + "/" + snapshot_name2)

        # Try to destroy snapshot with 2 clones and check if it fails
        ret_value = self.c.bdev_lvol_delete(snapshot_bdev['name'])
        if ret_value == 0:
            print("ERROR: Delete snapshot should fail but didn't")
            fail_count += 1

        # Destroy lvol store without deleting lvol bdevs
        fail_count += self.c.bdev_lvol_delete_lvstore(uuid_store)

        #  Check response bdev_lvol_get_lvstores command
        if self.c.check_bdev_lvol_get_lvstores("", "", "") == 1:
            fail_count += 1

        # Delete malloc bdev
        self.c.bdev_malloc_delete(base_name)
        # Expected result:
        # - bdev_lvol_get_lvstores: response should be of no value after destroyed lvol store
        # - no other operation fails
        return fail_count

    @case_message
    def test_case553(self):
        """
        unregister_lvol_bdev

        Test for unregistering the lvol bdevs.
        Removing malloc bdev under an lvol store triggers unregister of
        all lvol bdevs. Verify it with clones present.
        """

        fail_count = 0
        snapshot_name = "snapshot"
        snapshot_name2 = "snapshot2"
        clone_name = "clone"

        # Create malloc bdev
        base_name = self.c.bdev_malloc_create(self.total_size,
                                              self.block_size)
        # Construct_lvol_store on correct, exisitng malloc bdev
        uuid_store = self.c.bdev_lvol_create_lvstore(base_name,
                                                     self.lvs_name,
                                                     self.cluster_size)
        # Check correct uuid values in response bdev_lvol_get_lvstores command
        fail_count = self.c.check_bdev_lvol_get_lvstores(base_name, uuid_store,
                                                         self.cluster_size)
        lvs = self.c.bdev_lvol_get_lvstores()
        size = int(int(lvs[0]['free_clusters'] * lvs[0]['cluster_size']) / 4 / MEGABYTE)

        # Create lvol bdev, snapshot it, then clone it and then snapshot the clone
        uuid_bdev0 = self.c.bdev_lvol_create(uuid_store, self.lbd_name, size, thin=True)
        lvol_bdev = self.c.get_lvol_bdev_with_name(uuid_bdev0)

        fail_count += self.c.bdev_lvol_snapshot(lvol_bdev['name'], snapshot_name)
        snapshot_bdev = self.c.get_lvol_bdev_with_name(self.lvs_name + "/" + snapshot_name)

        fail_count += self.c.bdev_lvol_clone(self.lvs_name + "/" + snapshot_name, clone_name)
        clone_bdev = self.c.get_lvol_bdev_with_name(self.lvs_name + "/" + clone_name)

        fail_count += self.c.bdev_lvol_snapshot(clone_bdev['name'], snapshot_name2)
        snapshot_bdev2 = self.c.get_lvol_bdev_with_name(self.lvs_name + "/" + snapshot_name2)

        # Delete malloc bdev
        self.c.bdev_malloc_delete(base_name)

        #  Check response bdev_lvol_get_lvstores command
        if self.c.check_bdev_lvol_get_lvstores("", "", "") == 1:
            fail_count += 1

        # Expected result:
        # - bdev_lvol_get_lvstores: response should be of no value after destroyed lvol store
        # - no other operation fails
        return fail_count

    @case_message
    def test_case602(self):
        """
        bdev_lvol_create_lvstore_with_all_clear_methods

        Call bdev_lvol_create_lvstore with all options for clear methods.
        """
        fail_count = 0
        # Create malloc bdev
        base_name = self.c.bdev_malloc_create(self.total_size,
                                              self.block_size)
        # Construct lvol store with clear method 'none'
        lvol_uuid = self.c.bdev_lvol_create_lvstore(base_name, self.lvs_name, clear_method="none")
        fail_count += self.c.check_bdev_lvol_get_lvstores(base_name, lvol_uuid)
        fail_count += self.c.bdev_malloc_delete(base_name)

        # Create malloc bdev
        base_name = self.c.bdev_malloc_create(self.total_size,
                                              self.block_size)
        # Construct lvol store with clear method 'unmap'
        lvol_uuid = self.c.bdev_lvol_create_lvstore(base_name, self.lvs_name, clear_method="unmap")
        fail_count += self.c.check_bdev_lvol_get_lvstores(base_name, lvol_uuid)
        fail_count += self.c.bdev_malloc_delete(base_name)

        # Create malloc bdev
        base_name = self.c.bdev_malloc_create(self.total_size,
                                              self.block_size)
        # Construct lvol store with clear method 'write_zeroes'
        lvol_uuid = self.c.bdev_lvol_create_lvstore(base_name, self.lvs_name, clear_method="write_zeroes")
        fail_count += self.c.check_bdev_lvol_get_lvstores(base_name, lvol_uuid)
        fail_count += self.c.bdev_malloc_delete(base_name)

        # Expected result:
        # - construct lvol store return code != 0
        # - Error code response printed to stdout
        return fail_count

    @case_message
    def test_case650(self):
        """
        thin_provisioning_check_space

        Check if free clusters number on lvol store decreases
        if we write to created thin provisioned lvol bdev
        """
        # create malloc bdev
        base_name = self.c.bdev_malloc_create(self.total_size,
                                              self.block_size)
        # create lvol store on mamloc bdev
        uuid_store = self.c.bdev_lvol_create_lvstore(base_name,
                                                     self.lvs_name)
        fail_count = self.c.check_bdev_lvol_get_lvstores(base_name, uuid_store,
                                                         self.cluster_size)
        lvs = self.c.bdev_lvol_get_lvstores(self.lvs_name)[0]
        free_clusters_start = int(lvs['free_clusters'])
        bdev_size = self.get_lvs_size()
        # create thin provisioned lvol bdev with size equals to lvol store free space
        bdev_name = self.c.bdev_lvol_create(uuid_store, self.lbd_name,
                                            bdev_size, thin=True)
        lvs = self.c.bdev_lvol_get_lvstores(self.lvs_name)[0]
        free_clusters_create_lvol = int(lvs['free_clusters'])
        # check and save number of free clusters for lvol store
        if free_clusters_start != free_clusters_create_lvol:
            fail_count += 1
        lvol_bdev = self.c.get_lvol_bdev_with_name(bdev_name)
        nbd_name = "/dev/nbd0"
        fail_count += self.c.nbd_start_disk(bdev_name, nbd_name)

        size = int(lvs['cluster_size'])
        # write data (lvs cluster size) to created lvol bdev starting from offset 0.
        fail_count += self.run_fio_test("/dev/nbd0", 0, size, "write", "0xcc")
        lvs = self.c.bdev_lvol_get_lvstores(self.lvs_name)[0]
        free_clusters_first_fio = int(lvs['free_clusters'])
        # check that free clusters on lvol store was decremented by 1
        if free_clusters_start != free_clusters_first_fio + 1:
            fail_count += 1

        size = int(lvs['cluster_size'])
        # calculate size of one and half cluster
        offset = int((int(lvol_bdev['num_blocks']) * int(lvol_bdev['block_size']) /
                      free_clusters_create_lvol) * 1.5)
        # write data (lvs cluster size) to lvol bdev with offset set to one and half of cluster size
        fail_count += self.run_fio_test(nbd_name, offset, size, "write", "0xcc")
        lvs = self.c.bdev_lvol_get_lvstores(self.lvs_name)[0]
        free_clusters_second_fio = int(lvs['free_clusters'])
        # check that free clusters on lvol store was decremented by 2
        if free_clusters_start != free_clusters_second_fio + 3:
            fail_count += 1

        size = (free_clusters_create_lvol - 3) * int(lvs['cluster_size'])
        offset = int(int(lvol_bdev['num_blocks']) * int(lvol_bdev['block_size']) /
                     free_clusters_create_lvol * 3)
        # write data to lvol bdev to the end of its size
        fail_count += self.run_fio_test(nbd_name, offset, size, "write", "0xcc")
        lvs = self.c.bdev_lvol_get_lvstores(self.lvs_name)[0]
        free_clusters_third_fio = int(lvs['free_clusters'])
        # check that lvol store free clusters number equals to 0
        if free_clusters_third_fio != 0:
            fail_count += 1

        fail_count += self.c.nbd_stop_disk(nbd_name)
        # destroy thin provisioned lvol bdev
        fail_count += self.c.bdev_lvol_delete(lvol_bdev['name'])
        lvs = self.c.bdev_lvol_get_lvstores(self.lvs_name)[0]
        free_clusters_end = int(lvs['free_clusters'])
        # check that saved number of free clusters equals to current free clusters
        if free_clusters_start != free_clusters_end:
            fail_count += 1
        # destroy lvol store
        fail_count += self.c.bdev_lvol_delete_lvstore(uuid_store)
        # destroy malloc bdev
        fail_count += self.c.bdev_malloc_delete(base_name)
        # Expected result:
        # - calls successful, return code = 0
        # - no other operation fails
        return fail_count

    @case_message
    def test_case651(self):
        """
        thin_provisioning_read_empty_bdev

        Check if we can create thin provisioned bdev on empty lvol store
        and check if we can read from this device and it returns zeroes.
        """
        # create malloc bdev
        base_name = self.c.bdev_malloc_create(self.total_size,
                                              self.block_size)
        # construct lvol store on malloc bdev
        uuid_store = self.c.bdev_lvol_create_lvstore(base_name,
                                                     self.lvs_name)
        fail_count = self.c.check_bdev_lvol_get_lvstores(base_name, uuid_store,
                                                         self.cluster_size)
        lvs = self.c.bdev_lvol_get_lvstores(self.lvs_name)[0]
        free_clusters_start = int(lvs['free_clusters'])
        lbd_name0 = self.lbd_name + str("0")
        lbd_name1 = self.lbd_name + str("1")
        # calculate bdev size in megabytes
        bdev_size = self.get_lvs_size()
        # create thick provisioned lvol bvdev with size equal to lvol store
        bdev_name0 = self.c.bdev_lvol_create(uuid_store, lbd_name0,
                                             bdev_size, thin=False)
        # create thin provisioned lvol bdev with the same size
        bdev_name1 = self.c.bdev_lvol_create(uuid_store, lbd_name1,
                                             bdev_size, thin=True)
        lvol_bdev0 = self.c.get_lvol_bdev_with_name(bdev_name0)
        lvol_bdev1 = self.c.get_lvol_bdev_with_name(bdev_name1)
        nbd_name0 = "/dev/nbd0"
        fail_count += self.c.nbd_start_disk(lvol_bdev0['name'], nbd_name0)
        nbd_name1 = "/dev/nbd1"
        fail_count += self.c.nbd_start_disk(lvol_bdev1['name'], nbd_name1)

        size = bdev_size * MEGABYTE
        # fill the whole thick provisioned lvol bdev
        fail_count += self.run_fio_test(nbd_name0, 0, size, "write", False)

        size = bdev_size * MEGABYTE
        # perform read operations on thin provisioned lvol bdev
        # and check if they return zeroes
        fail_count += self.run_fio_test(nbd_name1, 0, size, "read", "0x00")

        fail_count += self.c.nbd_stop_disk(nbd_name0)
        fail_count += self.c.nbd_stop_disk(nbd_name1)
        # destroy thin provisioned lvol bdev
        fail_count += self.c.bdev_lvol_delete(lvol_bdev0['name'])
        fail_count += self.c.bdev_lvol_delete(lvol_bdev1['name'])
        # destroy lvol store
        fail_count += self.c.bdev_lvol_delete_lvstore(uuid_store)
        # destroy malloc bdev
        fail_count += self.c.bdev_malloc_delete(base_name)
        # Expected result:
        # - calls successful, return code = 0
        # - no other operation fails
        return fail_count

    @case_message
    def test_case652(self):
        """
        thin_provisioning_data_integrity_test

        Check if data written to thin provisioned lvol bdev
        were properly written (fio test with verification).
        """
        # create malloc bdev
        base_name = self.c.bdev_malloc_create(self.total_size,
                                              self.block_size)
        # construct lvol store on malloc bdev
        uuid_store = self.c.bdev_lvol_create_lvstore(base_name,
                                                     self.lvs_name)
        fail_count = self.c.check_bdev_lvol_get_lvstores(base_name, uuid_store,
                                                         self.cluster_size)
        lvs = self.c.bdev_lvol_get_lvstores(self.lvs_name)[0]
        free_clusters_start = int(lvs['free_clusters'])
        bdev_size = self.get_lvs_size()
        # construct thin provisioned lvol bdev with size equal to lvol store
        bdev_name = self.c.bdev_lvol_create(uuid_store, self.lbd_name,
                                            bdev_size, thin=True)

        lvol_bdev = self.c.get_lvol_bdev_with_name(bdev_name)
        nbd_name = "/dev/nbd0"
        fail_count += self.c.nbd_start_disk(lvol_bdev['name'], nbd_name)
        size = bdev_size * MEGABYTE
        # on the whole lvol bdev perform write operation with verification
        fail_count += self.run_fio_test(nbd_name, 0, size, "write", "0xcc")

        fail_count += self.c.nbd_stop_disk(nbd_name)
        # destroy thin provisioned lvol bdev
        fail_count += self.c.bdev_lvol_delete(lvol_bdev['name'])
        # destroy lvol store
        fail_count += self.c.bdev_lvol_delete_lvstore(uuid_store)
        # destroy malloc bdev
        fail_count += self.c.bdev_malloc_delete(base_name)
        # Expected result:
        # - calls successful, return code = 0
        # - verification ends with success
        # - no other operation fails
        return fail_count

    @case_message
    def test_case653(self):
        """
        thin_provisioning_resize

        Check thin provisioned bdev resize.
        """
        # Create malloc bdev
        base_name = self.c.bdev_malloc_create(self.total_size,
                                              self.block_size)
        # Construct lvol store on malloc bdev
        uuid_store = self.c.bdev_lvol_create_lvstore(base_name, self.lvs_name)
        fail_count = self.c.check_bdev_lvol_get_lvstores(base_name, uuid_store,
                                                         self.cluster_size)
        # Construct thin provisioned lvol bdevs on created lvol store
        # with size equal to 50% of lvol store
        size = self.get_lvs_divided_size(2)
        uuid_bdev = self.c.bdev_lvol_create(uuid_store,
                                            self.lbd_name, size, thin=True)
        fail_count += self.c.check_bdev_get_bdevs_methods(uuid_bdev, size)
        # Fill all free space of lvol bdev with data
        nbd_name = "/dev/nbd0"
        fail_count += self.c.nbd_start_disk(uuid_bdev, nbd_name)
        fail_count += self.run_fio_test(nbd_name, 0, size*MEGABYTE, "write", "0xcc", 0)
        fail_count += self.c.nbd_stop_disk(nbd_name)
        # Save number of free clusters for lvs
        lvs = self.c.bdev_lvol_get_lvstores()[0]
        free_clusters_start = int(lvs['free_clusters'])
        # Resize bdev to full size of lvs
        full_size = int(lvs['total_data_clusters'] * lvs['cluster_size'] / MEGABYTE)
        fail_count += self.c.bdev_lvol_resize(uuid_bdev, full_size)
        # Check if bdev size changed (total_data_clusters*cluster_size
        # equals to num_blocks*block_size)
        lvol_bdev = self.c.get_lvol_bdev_with_name(uuid_bdev)
        lbd_size = int(lvol_bdev['num_blocks'] * lvol_bdev['block_size'] / MEGABYTE)
        if full_size != lbd_size:
            fail_count += 1
        # Check if free_clusters on lvs remain unaffected
        lvs = self.c.bdev_lvol_get_lvstores()[0]
        free_clusters_resize = int(lvs['free_clusters'])
        if free_clusters_start != free_clusters_resize:
            fail_count += 1
        # Perform write operation with verification
        # to newly created free space of lvol bdev
        nbd_name = "/dev/nbd0"
        fail_count += self.c.nbd_start_disk(uuid_bdev, nbd_name)
        fail_count += self.run_fio_test(nbd_name, int(lbd_size * MEGABYTE / 2),
                                        int(lbd_size * MEGABYTE / 2), "write", "0xcc", 0)
        fail_count += self.c.nbd_stop_disk(nbd_name)
        # Check if free clusters on lvs equals to zero
        lvs = self.c.bdev_lvol_get_lvstores()[0]
        if int(lvs['free_clusters']) != 0:
            fail_count += 1
        # Resize bdev to 25% of lvs and check if it ended with success
        size = self.get_lvs_divided_size(4)
        fail_count += self.c.bdev_lvol_resize(uuid_bdev, size)
        # Check free clusters on lvs
        lvs = self.c.bdev_lvol_get_lvstores()[0]
        free_clusters_resize2 = int(lvs['free_clusters'])
        free_clusters_expected = int((full_size - size) * MEGABYTE / lvs['cluster_size'])
        if free_clusters_expected != free_clusters_resize2:
            fail_count += 1

        self.c.bdev_lvol_delete(uuid_bdev)
        self.c.bdev_lvol_delete_lvstore(uuid_store)
        self.c.bdev_malloc_delete(base_name)

        # Expected result:
        # - calls successful, return code = 0
        # - no other operation fails
        return fail_count

    @case_message
    def test_case654(self):
        """
        thin_overprovisioning

        Create two thin provisioned lvol bdevs with max size
        and check if writting more than total size of lvol store
        will cause failures.
        """
        # create malloc bdev
        base_name = self.c.bdev_malloc_create(self.total_size,
                                              self.block_size)
        # construct lvol store on malloc bdev
        uuid_store = self.c.bdev_lvol_create_lvstore(base_name, self.lvs_name)
        fail_count = self.c.check_bdev_lvol_get_lvstores(base_name, uuid_store,
                                                         self.cluster_size)
        lvs = self.c.bdev_lvol_get_lvstores(self.lvs_name)[0]
        free_clusters_start = int(lvs['free_clusters'])
        lbd_name0 = self.lbd_name + str("0")
        lbd_name1 = self.lbd_name + str("1")
        bdev_size = self.get_lvs_size()
        # construct two thin provisioned lvol bdevs on created lvol store
        # with size equals to free lvs size
        bdev_name0 = self.c.bdev_lvol_create(uuid_store, lbd_name0,
                                             bdev_size, thin=True)
        bdev_name1 = self.c.bdev_lvol_create(uuid_store, lbd_name1,
                                             bdev_size, thin=True)

        lvs = self.c.bdev_lvol_get_lvstores(self.lvs_name)[0]
        free_clusters_create_lvol = int(lvs['free_clusters'])
        if free_clusters_start != free_clusters_create_lvol:
            fail_count += 1
        lvol_bdev0 = self.c.get_lvol_bdev_with_name(bdev_name0)
        lvol_bdev1 = self.c.get_lvol_bdev_with_name(bdev_name1)

        nbd_name0 = "/dev/nbd0"
        nbd_name1 = "/dev/nbd1"
        fail_count += self.c.nbd_start_disk(lvol_bdev0['name'], nbd_name0)
        fail_count += self.c.nbd_start_disk(lvol_bdev1['name'], nbd_name1)

        size = "75%"
        # fill first bdev to 75% of its space with specific pattern
        fail_count += self.run_fio_test(nbd_name0, 0, size, "write", "0xcc")

        size = "75%"
        # fill second bdev up to 75% of its space
        # check that error message occured while filling second bdev with data
        fail_count += self.run_fio_test(nbd_name1, 0, size, "write", "0xee",
                                        expected_ret_value=1)

        size = "75%"
        # check if data on first disk stayed unchanged
        fail_count += self.run_fio_test(nbd_name0, 0, size, "read", "0xcc")

        size = "25%"
        offset = "75%"
        fail_count += self.run_fio_test(nbd_name0, offset, size, "read", "0x00")

        fail_count += self.c.nbd_stop_disk(nbd_name0)
        fail_count += self.c.nbd_stop_disk(nbd_name1)
        # destroy thin provisioned lvol bdev
        fail_count += self.c.bdev_lvol_delete(lvol_bdev0['name'])
        fail_count += self.c.bdev_lvol_delete(lvol_bdev1['name'])
        # destroy lvol store
        fail_count += self.c.bdev_lvol_delete_lvstore(uuid_store)
        # destroy malloc bdev
        fail_count += self.c.bdev_malloc_delete(base_name)
        # Expected result:
        # - calls successful, return code = 0
        # - no other operation fails
        return fail_count

    @case_message
    def test_case655(self):
        """
        thin_provisioning_filling_disks_less_than_lvs_size

        Check if writing to two thin provisioned lvol bdevs
        less than total size of lvol store will end with success
        """
        # create malloc bdev
        base_name = self.c.bdev_malloc_create(self.total_size,
                                              self.block_size)
        # construct lvol store on malloc bdev
        uuid_store = self.c.bdev_lvol_create_lvstore(base_name, self.lvs_name)
        fail_count = self.c.check_bdev_lvol_get_lvstores(base_name, uuid_store,
                                                         self.cluster_size)
        lvs = self.c.bdev_lvol_get_lvstores(self.lvs_name)[0]
        free_clusters_start = int(lvs['free_clusters'])
        lbd_name0 = self.lbd_name + str("0")
        lbd_name1 = self.lbd_name + str("1")
        lvs_size = self.get_lvs_size()
        bdev_size = int(lvs_size * 0.7)
        # construct two thin provisioned lvol bdevs on created lvol store
        # with size equal to 70% of lvs size
        bdev_name0 = self.c.bdev_lvol_create(uuid_store, lbd_name0,
                                             bdev_size, thin=True)
        bdev_name1 = self.c.bdev_lvol_create(uuid_store, lbd_name1,
                                             bdev_size, thin=True)

        lvol_bdev0 = self.c.get_lvol_bdev_with_name(bdev_name0)
        lvol_bdev1 = self.c.get_lvol_bdev_with_name(bdev_name1)
        # check if bdevs are available and size of every disk is equal to 70% of lvs size
        nbd_name0 = "/dev/nbd0"
        nbd_name1 = "/dev/nbd1"
        fail_count += self.c.nbd_start_disk(lvol_bdev0['name'], nbd_name0)
        fail_count += self.c.nbd_start_disk(lvol_bdev1['name'], nbd_name1)
        size = int(int(lvol_bdev0['num_blocks']) * int(lvol_bdev0['block_size']) * 0.7)
        # fill first disk with 70% of its size
        # check if operation didn't fail
        fail_count += self.run_fio_test(nbd_name0, 0, size, "write", "0xcc")
        size = int(int(lvol_bdev1['num_blocks']) * int(lvol_bdev1['block_size']) * 0.7)
        # fill second disk also with 70% of its size
        # check if operation didn't fail
        fail_count += self.run_fio_test(nbd_name1, 0, size, "write", "0xee")

        fail_count += self.c.nbd_stop_disk(nbd_name0)
        fail_count += self.c.nbd_stop_disk(nbd_name1)
        # destroy thin provisioned lvol bdevs
        fail_count += self.c.bdev_lvol_delete(lvol_bdev0['name'])
        fail_count += self.c.bdev_lvol_delete(lvol_bdev1['name'])
        # destroy lvol store
        fail_count += self.c.bdev_lvol_delete_lvstore(uuid_store)
        # destroy malloc bdev
        fail_count += self.c.bdev_malloc_delete(base_name)
        # Expected result:
        # - calls successful, return code = 0
        # - no other operation fails
        return fail_count

    @case_message
    def test_case700(self):
        """
        tasting_positive

        Positive test for tasting a multi lvol bdev configuration.
        Create a lvol store with some lvol bdevs on aio bdev and restart vhost app.
        After restarting configuration should be automatically loaded and should be exactly
        the same as before restarting.
        Check that running configuration can be modified after restarting and tasting.
        """
        fail_count = 0
        uuid_bdevs = []
        base_name = "aio_bdev0"

        base_path = path.dirname(sys.argv[0])
        vhost_path = path.join(self.app_path, 'vhost')
        pid_path = path.join(base_path, 'vhost.pid')
        aio_bdev0 = path.join(base_path, 'aio_bdev_0')

        self.c.bdev_aio_create(aio_bdev0, base_name, 4096)
        # Create initial configuration on running vhost instance
        # create lvol store, create 5 bdevs
        # save info of all lvs and lvol bdevs
        uuid_store = self.c.bdev_lvol_create_lvstore(base_name,
                                                     self.lvs_name)
        fail_count += self.c.check_bdev_lvol_get_lvstores(base_name,
                                                          uuid_store,
                                                          self.cluster_size)

        size = self.get_lvs_divided_size(10)

        for i in range(5):
            uuid_bdev = self.c.bdev_lvol_create(uuid_store,
                                                self.lbd_name + str(i),
                                                size)
            uuid_bdevs.append(uuid_bdev)
            # Using bdev_get_bdevs command verify lvol bdevs were correctly created
            fail_count += self.c.check_bdev_get_bdevs_methods(uuid_bdev, size)

        old_bdevs = sorted(self.c.get_lvol_bdevs(), key=lambda x: x["name"])
        old_stores = self.c.bdev_lvol_get_lvstores()

        # Shut down vhost instance and restart with new instance
        fail_count += self._stop_vhost(pid_path)
        remove(pid_path)
        if self._start_vhost(vhost_path, pid_path) != 0:
            fail_count += 1
            return fail_count

        self.c.bdev_aio_create(aio_bdev0, base_name, 4096)
        # Check if configuration was properly loaded after tasting
        # get all info all lvs and lvol bdevs, compare with previous info
        new_bdevs = sorted(self.c.get_lvol_bdevs(), key=lambda x: x["name"])
        new_stores = self.c.bdev_lvol_get_lvstores()

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
            self.c.bdev_aio_delete(aio_bdev0)
            return fail_count

        # Try modifying loaded configuration
        # Add some lvol bdevs to existing lvol store then
        # remove all lvol configuration and re-create it again
        for i in range(5, 10):
            uuid_bdev = self.c.bdev_lvol_create(uuid_store,
                                                self.lbd_name + str(i),
                                                size)
            uuid_bdevs.append(uuid_bdev)
            fail_count += self.c.check_bdev_get_bdevs_methods(uuid_bdev, size)

        for uuid_bdev in uuid_bdevs:
            self.c.bdev_lvol_delete(uuid_bdev)

        if self.c.bdev_lvol_delete_lvstore(uuid_store) != 0:
            fail_count += 1

        uuid_bdevs = []

        # Create lvol store on aio bdev, create ten lvol bdevs on lvol store and
        # verify all configuration call results
        uuid_store = self.c.bdev_lvol_create_lvstore(base_name,
                                                     self.lvs_name)
        fail_count += self.c.check_bdev_lvol_get_lvstores(base_name,
                                                          uuid_store,
                                                          self.cluster_size)

        for i in range(10):
            uuid_bdev = self.c.bdev_lvol_create(uuid_store,
                                                self.lbd_name + str(i),
                                                size)
            uuid_bdevs.append(uuid_bdev)
            fail_count += self.c.check_bdev_get_bdevs_methods(uuid_bdev, size)

        # Destroy lvol store
        if self.c.bdev_lvol_delete_lvstore(uuid_store) != 0:
            fail_count += 1

        self.c.bdev_aio_delete(base_name)

        return fail_count

    @case_message
    def test_case701(self):
        """
        tasting_lvol_store_positive

        Positive test for tasting lvol store.
        """
        base_path = path.dirname(sys.argv[0])
        aio_bdev0 = path.join(base_path, 'aio_bdev_0')
        base_name = "aio_bdev0"

        self.c.bdev_aio_create(aio_bdev0, base_name, 4096)
        # construct lvol store on aio bdev
        uuid_store = self.c.bdev_lvol_create_lvstore(base_name,
                                                     self.lvs_name)
        fail_count = self.c.check_bdev_lvol_get_lvstores(base_name, uuid_store,
                                                         self.cluster_size)

        self.c.bdev_aio_delete(base_name)
        self.c.bdev_aio_create(aio_bdev0, base_name, 4096)
        # wait 1 second to allow time for lvolstore tasting
        sleep(1)
        # check if lvol store still exists in vhost configuration
        if self.c.check_bdev_lvol_get_lvstores(base_name, uuid_store,
                                               self.cluster_size) != 0:
            fail_count += 1
        # destroy lvol store from aio bdev
        if self.c.bdev_lvol_delete_lvstore(uuid_store) != 0:
            fail_count += 1

        self.c.bdev_aio_delete(base_name)
        return fail_count

    @case_message
    def test_case702(self):
        """
        tasting_positive_with_different_lvol_store_cluster_size

        Positive test for tasting a multi lvol bdev configuration.
        Create two lvol stores with different cluster sizes with some lvol bdevs on aio
        drive and restart vhost app.
        After restarting configuration should be automatically loaded and should be exactly
        the same as before restarting.
        """
        fail_count = 0
        uuid_bdevs = []
        cluster_size_1M = MEGABYTE
        cluster_size_32M = 32 * MEGABYTE
        base_name_1M = "aio_bdev0"
        base_name_32M = "aio_bdev1"

        base_path = path.dirname(sys.argv[0])
        vhost_path = path.join(self.app_path, 'vhost')
        pid_path = path.join(base_path, 'vhost.pid')
        aio_bdev0 = path.join(base_path, 'aio_bdev_0')
        aio_bdev1 = path.join(base_path, 'aio_bdev_1')

        self.c.bdev_aio_create(aio_bdev0, base_name_1M, 4096)
        self.c.bdev_aio_create(aio_bdev1, base_name_32M, 4096)

        # Create initial configuration on running vhost instance
        # create lvol store, create 5 bdevs
        # save info of all lvs and lvol bdevs
        uuid_store_1M = self.c.bdev_lvol_create_lvstore(base_name_1M,
                                                        self.lvs_name + "_1M",
                                                        cluster_size_1M)

        fail_count += self.c.check_bdev_lvol_get_lvstores(base_name_1M,
                                                          uuid_store_1M,
                                                          cluster_size_1M)

        uuid_store_32M = self.c.bdev_lvol_create_lvstore(base_name_32M,
                                                         self.lvs_name + "_32M",
                                                         cluster_size_32M)

        fail_count += self.c.check_bdev_lvol_get_lvstores(base_name_32M,
                                                          uuid_store_32M,
                                                          cluster_size_32M)

        # size = approx 20% of total aio bdev size
        size_1M = self.get_lvs_divided_size(5, self.lvs_name + "_1M")
        size_32M = self.get_lvs_divided_size(5, self.lvs_name + "_32M")

        for i in range(5):
            uuid_bdev = self.c.bdev_lvol_create(uuid_store_1M,
                                                self.lbd_name + str(i) + "_1M",
                                                size_1M)
            uuid_bdevs.append(uuid_bdev)
            # Using bdev_get_bdevs command verify lvol bdevs were correctly created
            fail_count += self.c.check_bdev_get_bdevs_methods(uuid_bdev, size_1M)

        for i in range(5):
            uuid_bdev = self.c.bdev_lvol_create(uuid_store_32M,
                                                self.lbd_name + str(i) + "_32M",
                                                size_32M)
            uuid_bdevs.append(uuid_bdev)
            # Using bdev_get_bdevs command verify lvol bdevs were correctly created
            fail_count += self.c.check_bdev_get_bdevs_methods(uuid_bdev, size_32M)

        old_bdevs = sorted(self.c.get_lvol_bdevs(), key=lambda x: x["name"])
        old_stores = sorted(self.c.bdev_lvol_get_lvstores(), key=lambda x: x["name"])

        # Shut down vhost instance and restart with new instance
        fail_count += self._stop_vhost(pid_path)
        remove(pid_path)
        if self._start_vhost(vhost_path, pid_path) != 0:
            fail_count += 1
            return fail_count

        self.c.bdev_aio_create(aio_bdev0, base_name_1M, 4096)
        self.c.bdev_aio_create(aio_bdev1, base_name_32M, 4096)

        # wait 1 second to allow time for lvolstore tasting
        sleep(1)

        # Check if configuration was properly loaded after tasting
        # get all info all lvs and lvol bdevs, compare with previous info
        new_bdevs = sorted(self.c.get_lvol_bdevs(), key=lambda x: x["name"])
        new_stores = sorted(self.c.bdev_lvol_get_lvstores(), key=lambda x: x["name"])

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
            self.c.bdev_aio_delete(base_name_1M)
            self.c.bdev_aio_delete(base_name_32M)
            return fail_count

        for uuid_bdev in uuid_bdevs:
            self.c.bdev_lvol_delete(uuid_bdev)

        if self.c.bdev_lvol_delete_lvstore(uuid_store_1M) != 0:
            fail_count += 1

        if self.c.bdev_lvol_delete_lvstore(uuid_store_32M) != 0:
            fail_count += 1

        self.c.bdev_aio_delete(base_name_1M)
        self.c.bdev_aio_delete(base_name_32M)

        return fail_count

    @case_message
    def test_case750(self):
        """
        snapshot readonly

        Create snaphot of lvol bdev and check if it is readonly.
        """
        fail_count = 0
        nbd_name0 = "/dev/nbd0"
        snapshot_name = "snapshot0"
        # Construct malloc bdev
        base_name = self.c.bdev_malloc_create(self.total_size,
                                              self.block_size)
        # Construct lvol store on malloc bdev
        uuid_store = self.c.bdev_lvol_create_lvstore(base_name,
                                                     self.lvs_name)
        fail_count += self.c.check_bdev_lvol_get_lvstores(base_name, uuid_store,
                                                          self.cluster_size)

        lvs = self.c.bdev_lvol_get_lvstores()[0]
        free_clusters_start = int(lvs['free_clusters'])
        bdev_size = self.get_lvs_divided_size(3)
        # Create lvol bdev with 33% of lvol store space
        bdev_name = self.c.bdev_lvol_create(uuid_store, self.lbd_name,
                                            bdev_size)
        lvol_bdev = self.c.get_lvol_bdev_with_name(bdev_name)
        # Create snapshot of lvol bdev
        fail_count += self.c.bdev_lvol_snapshot(lvol_bdev['name'], snapshot_name)
        snapshot_bdev = self.c.get_lvol_bdev_with_name(self.lvs_name + "/" + snapshot_name)

        fail_count += self.c.nbd_start_disk(snapshot_bdev['name'], nbd_name0)
        size = bdev_size * MEGABYTE
        # Try to perform write operation on created snapshot
        # Check if filling snapshot of lvol bdev fails
        fail_count += self.run_fio_test(nbd_name0, 0, size, "write", "0xcc", 1)

        fail_count += self.c.nbd_stop_disk(nbd_name0)
        # Destroy lvol bdev
        fail_count += self.c.bdev_lvol_delete(lvol_bdev['name'])
        # Destroy snapshot
        fail_count += self.c.bdev_lvol_delete(snapshot_bdev['name'])
        # Destroy lvol store
        fail_count += self.c.bdev_lvol_delete_lvstore(uuid_store)
        # Delete malloc bdev
        fail_count += self.c.bdev_malloc_delete(base_name)

        # Expected result:
        # - calls successful, return code = 0
        # - no other operation fails
        return fail_count

    @case_message
    def test_case751(self):
        """
        snapshot_compare_with_lvol_bdev

        Check if lvol bdevs and snapshots contain the same data.
        Check if lvol bdev and snapshot differ when writing to lvol bdev
        after creating snapshot.
        """
        fail_count = 0
        nbd_name = ["/dev/nbd0", "/dev/nbd1", "/dev/nbd2", "/dev/nbd3"]
        snapshot_name0 = "snapshot0"
        snapshot_name1 = "snapshot1"
        # Construct mallov bdev
        base_name = self.c.bdev_malloc_create(self.total_size,
                                              self.block_size)
        # Construct lvol store
        uuid_store = self.c.bdev_lvol_create_lvstore(base_name,
                                                     self.lvs_name)
        fail_count += self.c.check_bdev_lvol_get_lvstores(base_name, uuid_store,
                                                          self.cluster_size)
        size = self.get_lvs_divided_size(6)
        lbd_name0 = self.lbd_name + str(0)
        lbd_name1 = self.lbd_name + str(1)
        # Create thin provisioned lvol bdev with size less than 25% of lvs
        uuid_bdev0 = self.c.bdev_lvol_create(uuid_store,
                                             lbd_name0, size, thin=True)
        # Create thick provisioned lvol bdev with size less than 25% of lvs
        uuid_bdev1 = self.c.bdev_lvol_create(uuid_store,
                                             lbd_name1, size, thin=False)
        lvol_bdev0 = self.c.get_lvol_bdev_with_name(uuid_bdev0)
        fail_count += self.c.nbd_start_disk(lvol_bdev0['name'], nbd_name[0])
        fill_size = int(size * MEGABYTE / 2)
        # Fill thin provisoned lvol bdev with 50% of its space
        fail_count += self.run_fio_test(nbd_name[0], 0, fill_size, "write", "0xcc", 0)
        lvol_bdev1 = self.c.get_lvol_bdev_with_name(uuid_bdev1)
        fail_count += self.c.nbd_start_disk(lvol_bdev1['name'], nbd_name[1])
        fill_size = int(size * MEGABYTE)
        # Fill whole thic provisioned lvol bdev
        fail_count += self.run_fio_test(nbd_name[1], 0, fill_size, "write", "0xcc", 0)

        # Create snapshots of lvol bdevs
        fail_count += self.c.bdev_lvol_snapshot(uuid_bdev0, snapshot_name0)
        fail_count += self.c.bdev_lvol_snapshot(uuid_bdev1, snapshot_name1)
        fail_count += self.c.nbd_start_disk(self.lvs_name + "/" + snapshot_name0, nbd_name[2])
        fail_count += self.c.nbd_start_disk(self.lvs_name + "/" + snapshot_name1, nbd_name[3])
        # Compare every lvol bdev with corresponding snapshot
        # and check that data are the same
        fail_count += self.compare_two_disks(nbd_name[0], nbd_name[2], 0)
        fail_count += self.compare_two_disks(nbd_name[1], nbd_name[3], 0)

        fill_size = int(size * MEGABYTE / 2)
        offset = fill_size
        # Fill second half of thin provisioned lvol bdev
        fail_count += self.run_fio_test(nbd_name[0], offset, fill_size, "write", "0xaa", 0)
        # Compare thin provisioned lvol bdev with its snapshot and check if it fails
        fail_count += self.compare_two_disks(nbd_name[0], nbd_name[2], 1)
        for nbd in nbd_name:
            fail_count += self.c.nbd_stop_disk(nbd)
        # Delete lvol bdevs
        fail_count += self.c.bdev_lvol_delete(lvol_bdev0['name'])
        fail_count += self.c.bdev_lvol_delete(lvol_bdev1['name'])
        # Delete snapshots
        fail_count += self.c.bdev_lvol_delete(self.lvs_name + "/" + snapshot_name0)
        fail_count += self.c.bdev_lvol_delete(self.lvs_name + "/" + snapshot_name1)
        # Destroy snapshot
        fail_count += self.c.bdev_lvol_delete_lvstore(uuid_store)
        # Delete malloc bdev
        fail_count += self.c.bdev_malloc_delete(base_name)

        # Expected result:
        # - calls successful, return code = 0
        # - removing snapshot should always end with success
        # - no other operation fails
        return fail_count

    @case_message
    def test_case752(self):
        """
        snapshot_during_io_traffic

        Check that when writing to lvol bdev
        creating snapshot ends with success
        """
        global current_fio_pid
        fail_count = 0
        nbd_name = "/dev/nbd0"
        snapshot_name = "snapshot"
        # Create malloc bdev
        base_name = self.c.bdev_malloc_create(self.total_size,
                                              self.block_size)
        # Construct lvol store
        uuid_store = self.c.bdev_lvol_create_lvstore(base_name,
                                                     self.lvs_name)
        fail_count += self.c.check_bdev_lvol_get_lvstores(base_name, uuid_store,
                                                          self.cluster_size)
        # Create thin provisioned lvol bdev with size equal to 50% of lvs space
        size = self.get_lvs_divided_size(2)
        uuid_bdev = self.c.bdev_lvol_create(uuid_store, self.lbd_name,
                                            size, thin=True)

        lvol_bdev = self.c.get_lvol_bdev_with_name(uuid_bdev)
        fail_count += self.c.nbd_start_disk(lvol_bdev['name'], nbd_name)
        fill_size = int(size * MEGABYTE)
        # Create thread that will run fio in background
        thread = FioThread(nbd_name, 0, fill_size, "write", "0xcc", 0,
                           extra_params="--time_based --runtime=8")
        # Perform write operation with verification to created lvol bdev
        thread.start()
        time.sleep(4)
        fail_count += is_process_alive(current_fio_pid)
        # During write operation create snapshot of created lvol bdev
        # and check that snapshot has been created successfully
        fail_count += self.c.bdev_lvol_snapshot(lvol_bdev['name'], snapshot_name)
        fail_count += is_process_alive(current_fio_pid)
        thread.join()
        # Check that write operation ended with success
        fail_count += thread.rv
        fail_count += self.c.nbd_stop_disk(nbd_name)
        # Destroy lvol bdev
        fail_count += self.c.bdev_lvol_delete(lvol_bdev['name'])
        # Delete snapshot
        fail_count += self.c.bdev_lvol_delete(self.lvs_name + "/" + snapshot_name)
        # Destroy lvol store
        fail_count += self.c.bdev_lvol_delete_lvstore(uuid_store)
        # Delete malloc bdevs
        fail_count += self.c.bdev_malloc_delete(base_name)

        # Expected result:
        # - calls successful, return code = 0
        # - no other operation fails
        return fail_count

    @case_message
    def test_case753(self):
        """
        snapshot_of_snapshot

        Check that creating snapshot of snapshot will fail
        """
        fail_count = 0
        snapshot_name0 = "snapshot0"
        snapshot_name1 = "snapshot1"
        # Create malloc bdev
        base_name = self.c.bdev_malloc_create(self.total_size,
                                              self.block_size)
        # Construct lvol store
        uuid_store = self.c.bdev_lvol_create_lvstore(base_name,
                                                     self.lvs_name)
        fail_count += self.c.check_bdev_lvol_get_lvstores(base_name, uuid_store,
                                                          self.cluster_size)
        # Create thick provisioned lvol bdev
        size = self.get_lvs_divided_size(2)
        uuid_bdev = self.c.bdev_lvol_create(uuid_store, self.lbd_name,
                                            size, thin=False)

        lvol_bdev = self.c.get_lvol_bdev_with_name(uuid_bdev)
        # Create snapshot of created lvol bdev
        fail_count += self.c.bdev_lvol_snapshot(lvol_bdev['name'], snapshot_name0)
        # Create snapshot of previously created snapshot
        # and check if operation will fail
        if self.c.bdev_lvol_snapshot(snapshot_name0, snapshot_name1) == 0:
            print("ERROR: Creating snapshot of snapshot should fail")
            fail_count += 1
        # Delete lvol bdev
        fail_count += self.c.bdev_lvol_delete(lvol_bdev['name'])
        # Destroy snapshot
        fail_count += self.c.bdev_lvol_delete(self.lvs_name + "/" + snapshot_name0)
        # Destroy lvol store
        fail_count += self.c.bdev_lvol_delete_lvstore(uuid_store)
        # Delete malloc bdev
        fail_count += self.c.bdev_malloc_delete(base_name)

        # Expected result:
        # - calls successful, return code = 0
        # - creating snapshot of snapshot should fail
        # - no other operation fails
        return fail_count

    @case_message
    def test_case754(self):
        """
        clone_bdev_only

        Check that only clone of snapshot can be created.
        Creating clone of lvol bdev should fail.
        """
        fail_count = 0
        clone_name = "clone"
        snapshot_name = "snapshot"
        # Create malloc bdev
        base_name = self.c.bdev_malloc_create(self.total_size,
                                              self.block_size)
        # Construct lvol store
        uuid_store = self.c.bdev_lvol_create_lvstore(base_name,
                                                     self.lvs_name)
        fail_count += self.c.check_bdev_lvol_get_lvstores(base_name, uuid_store,
                                                          self.cluster_size)
        lvs = self.c.bdev_lvol_get_lvstores()
        # Create thick provisioned lvol bdev with size equal to 50% of lvs space
        size = self.get_lvs_divided_size(2)
        uuid_bdev = self.c.bdev_lvol_create(uuid_store, self.lbd_name,
                                            size, thin=False)

        lvol_bdev = self.c.get_lvol_bdev_with_name(uuid_bdev)
        # Create clone of lvol bdev and check if it fails
        rv = self.c.bdev_lvol_clone(lvol_bdev['name'], clone_name)
        if rv == 0:
            print("ERROR: Creating clone of lvol bdev ended with unexpected success")
            fail_count += 1
        # Create snapshot of lvol bdev
        fail_count += self.c.bdev_lvol_snapshot(lvol_bdev['name'], snapshot_name)
        # Create again clone of lvol bdev and check if it fails
        rv = self.c.bdev_lvol_clone(lvol_bdev['name'], clone_name)
        if rv == 0:
            print("ERROR: Creating clone of lvol bdev ended with unexpected success")
            fail_count += 1
        # Create clone of snapshot and check if it ends with success
        rv = self.c.bdev_lvol_clone(self.lvs_name + "/" + snapshot_name, clone_name)
        if rv != 0:
            print("ERROR: Creating clone of snapshot ended with unexpected failure")
            fail_count += 1
        clone_bdev = self.c.get_lvol_bdev_with_name(self.lvs_name + "/" + clone_name)

        # Delete lvol bdev
        fail_count += self.c.bdev_lvol_delete(lvol_bdev['name'])
        # Destroy clone
        fail_count += self.c.bdev_lvol_delete(clone_bdev['name'])
        # Delete snapshot
        fail_count += self.c.bdev_lvol_delete(self.lvs_name + "/" + snapshot_name)
        # Delete lvol store
        fail_count += self.c.bdev_lvol_delete_lvstore(uuid_store)
        # Destroy malloc bdev
        fail_count += self.c.bdev_malloc_delete(base_name)

        # Expected result:
        # - calls successful, return code = 0
        # - cloning thick provisioned lvol bdev should fail
        # - no other operation fails
        return fail_count

    @case_message
    def test_case755(self):
        """
        clone_writing_to_clone


        """
        fail_count = 0
        nbd_name = ["/dev/nbd0", "/dev/nbd1", "/dev/nbd2", "/dev/nbd3"]
        snapshot_name = "snapshot"
        clone_name0 = "clone0"
        clone_name1 = "clone1"
        # Create malloc bdev
        base_name = self.c.bdev_malloc_create(self.total_size,
                                              self.block_size)
        # Create lvol store
        uuid_store = self.c.bdev_lvol_create_lvstore(base_name,
                                                     self.lvs_name)
        fail_count += self.c.check_bdev_lvol_get_lvstores(base_name, uuid_store,
                                                          self.cluster_size)
        size = self.get_lvs_divided_size(6)
        lbd_name0 = self.lbd_name + str(0)
        # Construct thick provisioned lvol bdev
        uuid_bdev0 = self.c.bdev_lvol_create(uuid_store,
                                             lbd_name0, size, thin=False)
        lvol_bdev = self.c.get_lvol_bdev_with_name(uuid_bdev0)
        # Install lvol bdev on /dev/nbd0
        fail_count += self.c.nbd_start_disk(lvol_bdev['name'], nbd_name[0])
        fill_size = size * MEGABYTE
        # Fill lvol bdev with 100% of its space
        fail_count += self.run_fio_test(nbd_name[0], 0, fill_size, "write", "0xcc", 0)

        # Create snapshot of thick provisioned lvol bdev
        fail_count += self.c.bdev_lvol_snapshot(lvol_bdev['name'], snapshot_name)
        snapshot_bdev = self.c.get_lvol_bdev_with_name(self.lvs_name + "/" + snapshot_name)
        # Create two clones of created snapshot
        fail_count += self.c.bdev_lvol_clone(snapshot_bdev['name'], clone_name0)
        fail_count += self.c.bdev_lvol_clone(snapshot_bdev['name'], clone_name1)

        lvol_clone0 = self.c.get_lvol_bdev_with_name(self.lvs_name + "/" + clone_name0)
        fail_count += self.c.nbd_start_disk(lvol_clone0['name'], nbd_name[1])
        fill_size = int(size * MEGABYTE / 2)
        # Perform write operation to first clone
        # Change first half of its space
        fail_count += self.run_fio_test(nbd_name[1], 0, fill_size, "write", "0xaa", 0)
        fail_count += self.c.nbd_start_disk(self.lvs_name + "/" + snapshot_name, nbd_name[2])
        lvol_clone1 = self.c.get_lvol_bdev_with_name(self.lvs_name + "/" + clone_name1)
        fail_count += self.c.nbd_start_disk(lvol_clone1['name'], nbd_name[3])
        # Compare snapshot with second clone. Data on both bdevs should be the same
        time.sleep(1)
        fail_count += self.compare_two_disks(nbd_name[2], nbd_name[3], 0)

        for nbd in nbd_name:
            fail_count += self.c.nbd_stop_disk(nbd)
        # Destroy lvol bdev
        fail_count += self.c.bdev_lvol_delete(lvol_bdev['name'])
        # Destroy two clones
        fail_count += self.c.bdev_lvol_delete(lvol_clone0['name'])
        fail_count += self.c.bdev_lvol_delete(lvol_clone1['name'])
        # Delete snapshot
        fail_count += self.c.bdev_lvol_delete(snapshot_bdev['name'])
        # Destroy lvol store
        fail_count += self.c.bdev_lvol_delete_lvstore(uuid_store)
        # Delete malloc
        fail_count += self.c.bdev_malloc_delete(base_name)

        # Expected result:
        # - calls successful, return code = 0
        # - no other operation fails
        return fail_count

    @case_message
    def test_case756(self):
        """
        clone_and_snapshot_relations

        Check if relations between clones and snapshots
        are properly set in configuration
        """
        fail_count = 0
        snapshot_name = 'snapshot'
        clone_name0 = 'clone1'
        clone_name1 = 'clone2'
        lbd_name = clone_name1

        # Create malloc bdev
        base_name = self.c.bdev_malloc_create(self.total_size,
                                              self.block_size)
        # Create lvol store
        uuid_store = self.c.bdev_lvol_create_lvstore(base_name,
                                                     self.lvs_name)
        fail_count += self.c.check_bdev_lvol_get_lvstores(base_name, uuid_store,
                                                          self.cluster_size)
        size = self.get_lvs_divided_size(6)

        # Construct thick provisioned lvol bdev
        uuid_bdev = self.c.bdev_lvol_create(uuid_store,
                                            lbd_name, size, thin=False)
        lvol_bdev = self.c.get_lvol_bdev_with_name(uuid_bdev)

        # Create snapshot of thick provisioned lvol bdev
        fail_count += self.c.bdev_lvol_snapshot(lvol_bdev['name'], snapshot_name)
        snapshot_bdev = self.c.get_lvol_bdev_with_name(self.lvs_name + "/" + snapshot_name)

        # Create clone of created snapshot
        fail_count += self.c.bdev_lvol_clone(snapshot_bdev['name'], clone_name0)

        # Get current bdevs configuration
        snapshot_bdev = self.c.get_lvol_bdev_with_name(self.lvs_name + "/" + snapshot_name)
        lvol_clone0 = self.c.get_lvol_bdev_with_name(self.lvs_name + "/" + clone_name0)
        lvol_clone1 = self.c.get_lvol_bdev_with_name(self.lvs_name + "/" + clone_name1)

        # Check snapshot consistency
        snapshot_lvol = snapshot_bdev['driver_specific']['lvol']
        if snapshot_lvol['snapshot'] is not True:
            fail_count += 1
        if snapshot_lvol['clone'] is not False:
            fail_count += 1
        if sorted([clone_name0, clone_name1]) != sorted(snapshot_lvol['clones']):
            fail_count += 1

        # Check first clone consistency
        lvol_clone0_lvol = lvol_clone0['driver_specific']['lvol']
        if lvol_clone0_lvol['snapshot'] is not False:
            fail_count += 1
        if lvol_clone0_lvol['clone'] is not True:
            fail_count += 1
        if lvol_clone0_lvol['base_snapshot'] != 'snapshot':
            fail_count += 1

        # Check second clone consistency
        lvol_clone1_lvol = lvol_clone1['driver_specific']['lvol']
        if lvol_clone1_lvol['snapshot'] is not False:
            fail_count += 1
        if lvol_clone1_lvol['clone'] is not True:
            fail_count += 1
        if lvol_clone1_lvol['base_snapshot'] != 'snapshot':
            fail_count += 1

        # Destroy first clone and check if it is deleted from snapshot
        fail_count += self.c.bdev_lvol_delete(lvol_clone0['name'])
        snapshot_bdev = self.c.get_lvol_bdev_with_name(self.lvs_name + "/" + snapshot_name)
        if [clone_name1] != snapshot_bdev['driver_specific']['lvol']['clones']:
            fail_count += 1

        # Destroy second clone
        fail_count += self.c.bdev_lvol_delete(lvol_clone1['name'])

        # Delete snapshot
        fail_count += self.c.bdev_lvol_delete(snapshot_bdev['name'])

        # Destroy lvol store
        fail_count += self.c.bdev_lvol_delete_lvstore(uuid_store)

        # Delete malloc
        fail_count += self.c.bdev_malloc_delete(base_name)

        # Expected result:
        # - calls successful, return code = 0
        # - no other operation fails
        return fail_count

    @case_message
    def test_case757(self):
        """
        clone_inflate


        Test inflate rpc method
        """
        fail_count = 0
        snapshot_name = "snapshot"
        nbd_name = "/dev/nbd0"

        # Create malloc bdev
        base_name = self.c.bdev_malloc_create(self.total_size,
                                              self.block_size)

        # Create lvol store
        uuid_store = self.c.bdev_lvol_create_lvstore(base_name,
                                                     self.lvs_name)
        fail_count += self.c.check_bdev_lvol_get_lvstores(base_name, uuid_store,
                                                          self.cluster_size)
        size = self.get_lvs_divided_size(4)

        # Construct thick provisioned lvol bdev
        uuid_bdev0 = self.c.bdev_lvol_create(uuid_store,
                                             self.lbd_name, size, thin=False)
        lvol_bdev = self.c.get_lvol_bdev_with_name(uuid_bdev0)

        # Fill bdev with data of knonw pattern
        fail_count += self.c.nbd_start_disk(lvol_bdev['name'], nbd_name)
        fill_size = size * MEGABYTE
        fail_count += self.run_fio_test(nbd_name, 0, fill_size, "write", "0xcc", 0)
        self.c.nbd_stop_disk(nbd_name)

        # Create snapshot of thick provisioned lvol bdev
        fail_count += self.c.bdev_lvol_snapshot(lvol_bdev['name'], snapshot_name)
        snapshot_bdev = self.c.get_lvol_bdev_with_name(self.lvs_name + "/" + snapshot_name)

        # Create two clones of created snapshot
        lvol_clone = self.c.get_lvol_bdev_with_name(self.lvs_name + "/" + self.lbd_name)
        if lvol_clone['driver_specific']['lvol']['thin_provision'] is not True:
            fail_count += 1

        # Fill part of clone with data of known pattern
        fail_count += self.c.nbd_start_disk(lvol_clone['name'], nbd_name)
        first_fill = 0
        second_fill = int(size * 3 / 4)
        fail_count += self.run_fio_test(nbd_name, first_fill * MEGABYTE,
                                        MEGABYTE, "write", "0xdd", 0)
        fail_count += self.run_fio_test(nbd_name, second_fill * MEGABYTE,
                                        MEGABYTE, "write", "0xdd", 0)
        self.c.nbd_stop_disk(nbd_name)

        # Do inflate
        fail_count += self.c.bdev_lvol_inflate(lvol_clone['name'])
        lvol_clone = self.c.get_lvol_bdev_with_name(self.lvs_name + "/" + self.lbd_name)
        if lvol_clone['driver_specific']['lvol']['thin_provision'] is not False:
            fail_count += 1

        # Delete snapshot
        fail_count += self.c.bdev_lvol_delete(snapshot_bdev['name'])

        # Check data consistency
        fail_count += self.c.nbd_start_disk(lvol_clone['name'], nbd_name)
        fail_count += self.run_fio_test(nbd_name, first_fill * MEGABYTE,
                                        MEGABYTE, "read", "0xdd")
        fail_count += self.run_fio_test(nbd_name, (first_fill + 1) * MEGABYTE,
                                        (second_fill - first_fill - 1) * MEGABYTE,
                                        "read", "0xcc")
        fail_count += self.run_fio_test(nbd_name, (second_fill) * MEGABYTE,
                                        MEGABYTE, "read", "0xdd")
        fail_count += self.run_fio_test(nbd_name, (second_fill + 1) * MEGABYTE,
                                        (size - second_fill - 1) * MEGABYTE,
                                        "read", "0xcc")
        self.c.nbd_stop_disk(nbd_name)

        # Destroy lvol bdev
        fail_count += self.c.bdev_lvol_delete(lvol_bdev['name'])

        # Destroy lvol store
        fail_count += self.c.bdev_lvol_delete_lvstore(uuid_store)

        # Delete malloc
        fail_count += self.c.bdev_malloc_delete(base_name)

        # Expected result:
        # - calls successful, return code = 0
        # - no other operation fails
        return fail_count

    @case_message
    def test_case758(self):
        """
        clone_decouple_parent

        Detach parent from clone and check if parent can be safely removed.
        Check data consistency.
        """

        fail_count = 0
        snapshot_name = "snapshot"
        nbd_name = "/dev/nbd0"

        # Create malloc bdev
        base_name = self.c.bdev_malloc_create(self.total_size,
                                              self.block_size)

        # Create lvol store
        uuid_store = self.c.bdev_lvol_create_lvstore(base_name,
                                                     self.lvs_name)
        fail_count += self.c.check_bdev_lvol_get_lvstores(base_name, uuid_store,
                                                          self.cluster_size)
        size = self.get_lvs_divided_size(4)

        # Construct thin provisioned lvol bdev
        uuid_bdev0 = self.c.bdev_lvol_create(uuid_store,
                                             self.lbd_name, size, thin=True)
        lvol_bdev = self.c.get_lvol_bdev_with_name(uuid_bdev0)

        # Decouple parent lvol bdev and check if it fails
        ret_value = self.c.bdev_lvol_decouple_parent(lvol_bdev['name'])
        if ret_value == 0:
            print("ERROR: Decouple parent on bdev without parent should "
                  "fail but didn't")
            fail_count += 1

        # Create snapshot of thin provisioned lvol bdev
        fail_count += self.c.bdev_lvol_snapshot(lvol_bdev['name'], snapshot_name)
        snapshot_bdev = self.c.get_lvol_bdev_with_name(self.lvs_name + "/" + snapshot_name)

        # Decouple parent lvol bdev
        fail_count += self.c.bdev_lvol_decouple_parent(lvol_bdev['name'])
        lvol_bdev = self.c.get_lvol_bdev_with_name(uuid_bdev0)
        snapshot_bdev = self.c.get_lvol_bdev_with_name(self.lvs_name + "/" + snapshot_name)
        if lvol_bdev['driver_specific']['lvol']['thin_provision'] is not True:
            fail_count += 1
        if lvol_bdev['driver_specific']['lvol']['clone'] is not False:
            fail_count += 1
        if lvol_bdev['driver_specific']['lvol']['snapshot'] is not False:
            fail_count += 1
        if snapshot_bdev['driver_specific']['lvol']['clone'] is not False:
            fail_count += 1

        # Destroy snapshot
        fail_count += self.c.bdev_lvol_delete(snapshot_bdev['name'])

        # Destroy lvol bdev
        fail_count += self.c.bdev_lvol_delete(lvol_bdev['name'])

        # Destroy lvol store
        fail_count += self.c.bdev_lvol_delete_lvstore(uuid_store)

        # Delete malloc
        fail_count += self.c.bdev_malloc_delete(base_name)

        # Expected result:
        # - calls successful, return code = 0
        # - no other operation fails
        return fail_count

    @case_message
    def test_case759(self):
        """
        clone_decouple_parent_rw

        Create tree level snaphot-snapshot2-clone structure.
        Detach snapshot2 from clone. Check if snapshot2 can be safely removed.
        Each time check consistency of snapshot-clone relations and written data.
        """
        fail_count = 0
        snapshot_name = "snapshot"
        snapshot_name2 = "snapshot2"
        nbd_name = "/dev/nbd0"

        # Create malloc bdev
        base_name = self.c.bdev_malloc_create(self.total_size,
                                              self.block_size)

        # Create lvol store
        uuid_store = self.c.bdev_lvol_create_lvstore(base_name,
                                                     self.lvs_name)
        fail_count += self.c.check_bdev_lvol_get_lvstores(base_name, uuid_store,
                                                          self.cluster_size)
        lvs = self.c.bdev_lvol_get_lvstores()
        size = int(5 * lvs[0]['cluster_size'] / MEGABYTE)

        # Construct thin provisioned lvol bdev
        uuid_bdev0 = self.c.bdev_lvol_create(uuid_store,
                                             self.lbd_name, size, thin=True)
        lvol_bdev = self.c.get_lvol_bdev_with_name(uuid_bdev0)

        # Fill first four out of 5 culsters of clone with data of known pattern
        fail_count += self.c.nbd_start_disk(lvol_bdev['name'], nbd_name)
        begin_fill = 0
        end_fill = int(size * 4 / 5)
        fail_count += self.run_fio_test(nbd_name, begin_fill * MEGABYTE,
                                        end_fill * MEGABYTE, "write", "0xdd", 0)

        # Create snapshot of thin provisioned lvol bdev
        fail_count += self.c.bdev_lvol_snapshot(lvol_bdev['name'], snapshot_name)
        snapshot_bdev = self.c.get_lvol_bdev_with_name(self.lvs_name + "/" + snapshot_name)

        # Fill second and fourth cluster of clone with data of known pattern
        start_fill = int(size / 5)
        fill_range = int(size / 5)
        fail_count += self.run_fio_test(nbd_name, start_fill * MEGABYTE,
                                        fill_range * MEGABYTE, "write", "0xcc", 0)
        start_fill = int(size * 3 / 5)
        fail_count += self.run_fio_test(nbd_name, start_fill * MEGABYTE,
                                        fill_range * MEGABYTE, "write", "0xcc", 0)

        # Create second snapshot of thin provisioned lvol bdev
        fail_count += self.c.bdev_lvol_snapshot(lvol_bdev['name'], snapshot_name2)
        snapshot_bdev2 = self.c.get_lvol_bdev_with_name(self.lvs_name + "/" + snapshot_name2)

        # Fill second cluster of clone with data of known pattern
        start_fill = int(size / 5)
        fail_count += self.run_fio_test(nbd_name, start_fill * MEGABYTE,
                                        fill_range * MEGABYTE, "write", "0xee", 0)

        # Check data consistency
        pattern = ["0xdd", "0xee", "0xdd", "0xcc", "0x00"]
        for i in range(0, 5):
            begin_fill = int(size * i / 5)
            fail_count += self.run_fio_test(nbd_name, begin_fill * MEGABYTE,
                                            fill_range * MEGABYTE, "read", pattern[i])

        # Decouple parent
        fail_count += self.c.bdev_lvol_decouple_parent(lvol_bdev['name'])
        lvol_bdev = self.c.get_lvol_bdev_with_name(uuid_bdev0)

        # Check data consistency
        for i in range(0, 5):
            begin_fill = int(size * i / 5)
            fail_count += self.run_fio_test(nbd_name, begin_fill * MEGABYTE,
                                            fill_range * MEGABYTE, "read", pattern[i])

        # Delete second snapshot
        ret_value = self.c.bdev_lvol_delete(snapshot_bdev2['name'])

        # Check data consistency
        for i in range(0, 5):
            begin_fill = int(size * i / 5)
            fail_count += self.run_fio_test(nbd_name, begin_fill * MEGABYTE,
                                            fill_range * MEGABYTE, "read", pattern[i])

        # Destroy lvol bdev
        fail_count += self.c.bdev_lvol_delete(lvol_bdev['name'])

        # Destroy snapshot
        fail_count += self.c.bdev_lvol_delete(snapshot_bdev['name'])

        # Destroy lvol store
        fail_count += self.c.bdev_lvol_delete_lvstore(uuid_store)

        # Delete malloc
        fail_count += self.c.bdev_malloc_delete(base_name)

        # Expected result:
        # - calls successful, return code = 0
        # - no other operation fails
        return fail_count

    @case_message
    def test_case760(self):
        """
        set read only

        Set lvol bdev as read only and perform clone on it.
        """
        fail_count = 0
        nbd_name0 = "/dev/nbd0"
        nbd_name1 = "/dev/nbd1"
        clone_name = "clone0"
        # Construct malloc bdev
        base_name = self.c.bdev_malloc_create(self.total_size,
                                              self.block_size)
        # Construct lvol store on malloc bdev
        uuid_store = self.c.bdev_lvol_create_lvstore(base_name,
                                                     self.lvs_name)
        fail_count += self.c.check_bdev_lvol_get_lvstores(base_name, uuid_store,
                                                          self.cluster_size)

        # Create lvol bdev with 50% of lvol store space
        lvs = self.c.bdev_lvol_get_lvstores()[0]
        free_clusters_start = int(lvs['free_clusters'])
        bdev_size = self.get_lvs_divided_size(2)
        bdev_name = self.c.bdev_lvol_create(uuid_store, self.lbd_name,
                                            bdev_size)
        # Set lvol bdev as read only
        lvol_bdev = self.c.get_lvol_bdev_with_name(bdev_name)
        fail_count += self.c.bdev_lvol_set_read_only(lvol_bdev['name'])

        # Try to perform write operation on lvol marked as read only
        fail_count += self.c.nbd_start_disk(lvol_bdev['name'], nbd_name0)
        size = bdev_size * MEGABYTE
        fail_count += self.run_fio_test(nbd_name0, 0, size, "write", "0xcc", 1)

        # Create clone of lvol set to read only
        rv = self.c.bdev_lvol_clone(lvol_bdev['name'], clone_name)
        if rv != 0:
            print("ERROR: Creating clone of snapshot ended with unexpected failure")
            fail_count += 1
        clone_bdev = self.c.get_lvol_bdev_with_name(self.lvs_name + "/" + clone_name)

        # Try to perform write operation on lvol clone
        fail_count += self.c.nbd_start_disk(clone_bdev['name'], nbd_name1)
        size = bdev_size * MEGABYTE
        fail_count += self.run_fio_test(nbd_name1, 0, size, "write", "0xcc", 0)

        # Stop nbd disks
        fail_count += self.c.nbd_stop_disk(nbd_name0)
        fail_count += self.c.nbd_stop_disk(nbd_name1)
        # Destroy clone lvol bdev
        fail_count += self.c.bdev_lvol_delete(clone_bdev['name'])
        # Destroy lvol bdev
        fail_count += self.c.bdev_lvol_delete(lvol_bdev['name'])
        # Destroy lvol store
        fail_count += self.c.bdev_lvol_delete_lvstore(uuid_store)
        # Delete malloc bdev
        fail_count += self.c.bdev_malloc_delete(base_name)

        # Expected result:
        # - calls successful, return code = 0
        # - no other operation fails
        return fail_count

    @case_message
    def test_case761(self):
        """
        delete_snapshot

        Check if it is possible to delete snapshot with clone
        """
        fail_count = 0
        nbd_name0 = "/dev/nbd0"
        nbd_name1 = "/dev/nbd1"
        snapshot_name = "snapshot"
        # Construct malloc bdev
        base_name = self.c.bdev_malloc_create(self.total_size,
                                              self.block_size)
        # Construct lvol store on malloc bdev
        uuid_store = self.c.bdev_lvol_create_lvstore(base_name,
                                                     self.lvs_name)
        fail_count += self.c.check_bdev_lvol_get_lvstores(base_name, uuid_store,
                                                          self.cluster_size)

        # Create lvol bdev with 50% of lvol store space
        lvs = self.c.bdev_lvol_get_lvstores()[0]
        bdev_size = self.get_lvs_divided_size(2)
        bdev_name = self.c.bdev_lvol_create(uuid_store, self.lbd_name,
                                            bdev_size)
        lvol_bdev = self.c.get_lvol_bdev_with_name(bdev_name)

        # Perform write operation on lvol
        fail_count += self.c.nbd_start_disk(lvol_bdev['name'], nbd_name0)
        size = bdev_size * MEGABYTE
        fail_count += self.run_fio_test(nbd_name0, 0, size, "write", "0xcc")

        # Create snapshot of lvol bdev
        fail_count += self.c.bdev_lvol_snapshot(lvol_bdev['name'], snapshot_name)
        snapshot_bdev = self.c.get_lvol_bdev_with_name(self.lvs_name + "/" + snapshot_name)
        if snapshot_bdev['driver_specific']['lvol']['clone'] is not False\
                or snapshot_bdev['driver_specific']['lvol']['snapshot'] is not True:
            fail_count += 1

        # Fill first half of lvol bdev
        half_size = bdev_size * MEGABYTE / 2
        lvol_bdev = self.c.get_lvol_bdev_with_name(bdev_name)
        fail_count += self.run_fio_test(nbd_name0, 0, half_size-1, "write", "0xee")

        # Check if snapshot was unchanged
        fail_count += self.c.nbd_start_disk(snapshot_bdev['name'], nbd_name1)
        fail_count += self.run_fio_test(nbd_name1, 0, half_size-1, "read", "0xcc")

        # Verify lvol bdev
        fail_count += self.run_fio_test(nbd_name0, 0, half_size-1, "read", "0xee")
        if lvol_bdev['driver_specific']['lvol']['clone'] is not True:
            fail_count += 1

        # Delete snapshot - should succeed
        fail_count += self.c.nbd_stop_disk(nbd_name1)
        fail_count += self.c.bdev_lvol_delete(snapshot_bdev['name'])

        # Check data consistency
        lvol_bdev = self.c.get_lvol_bdev_with_name(bdev_name)
        if lvol_bdev['driver_specific']['lvol']['clone'] is not False:
            fail_count += 1
        fail_count += self.run_fio_test(nbd_name0, 0, half_size-1, "read", "0xee")
        fail_count += self.run_fio_test(nbd_name0, half_size, size-1, "read", "0xcc")

        # Destroy lvol bdev
        fail_count += self.c.nbd_stop_disk(nbd_name0)
        fail_count += self.c.bdev_lvol_delete(lvol_bdev['name'])

        # Destroy lvol store
        fail_count += self.c.bdev_lvol_delete_lvstore(uuid_store)
        # Delete malloc bdev
        fail_count += self.c.bdev_malloc_delete(base_name)

        # Expected result:
        # - calls successful, return code = 0
        # - no other operation fails
        return fail_count

    @case_message
    def test_case762(self):
        """
        delete_snapshot_with_snapshot

        Check if it is possible to delete snapshot with one snapshot on it
        """
        fail_count = 0
        nbd_name0 = "/dev/nbd0"
        nbd_name1 = "/dev/nbd1"
        nbd_name2 = "/dev/nbd2"
        snapshot_name = "snapshot"
        snapshot_name2 = "snapshot2"
        # Construct malloc bdev
        base_name = self.c.bdev_malloc_create(self.total_size,
                                              self.block_size)
        # Construct lvol store on malloc bdev
        uuid_store = self.c.bdev_lvol_create_lvstore(base_name,
                                                     self.lvs_name)
        fail_count += self.c.check_bdev_lvol_get_lvstores(base_name, uuid_store,
                                                          self.cluster_size)

        # Create lvol bdev with one third of lvol store space
        lvs = self.c.bdev_lvol_get_lvstores()[0]
        bdev_size = self.get_lvs_divided_size(3)
        bdev_name = self.c.bdev_lvol_create(uuid_store, self.lbd_name,
                                            bdev_size)
        lvol_bdev = self.c.get_lvol_bdev_with_name(bdev_name)

        # Perform write operation on lvol
        fail_count += self.c.nbd_start_disk(lvol_bdev['name'], nbd_name0)
        size = bdev_size * MEGABYTE
        fail_count += self.run_fio_test(nbd_name0, 0, size-1, "write", "0xcc")

        # Create snapshot of lvol bdev
        fail_count += self.c.bdev_lvol_snapshot(lvol_bdev['name'], snapshot_name)
        snapshot_bdev = self.c.get_lvol_bdev_with_name(self.lvs_name + "/" + snapshot_name)
        lvol_bdev = self.c.get_lvol_bdev_with_name(bdev_name)
        if lvol_bdev['driver_specific']['lvol']['base_snapshot'] != snapshot_name:
            fail_count += 1

        # Fill second 1/3 of lvol bdev
        first_part = int(size / 3)
        second_part = int(size / 3 * 2)
        fail_count += self.run_fio_test(nbd_name0, first_part, second_part-first_part, "write", "0xee")

        # Check if snapshot was unchanged
        fail_count += self.c.nbd_start_disk(snapshot_bdev['name'], nbd_name1)
        fail_count += self.run_fio_test(nbd_name1, 0, size-1, "read", "0xcc")

        # Create second snapshot of lvol_bdev
        # First snapshot becomes snapshot of second snapshot
        fail_count += self.c.bdev_lvol_snapshot(lvol_bdev['name'], snapshot_name2)
        snapshot_bdev2 = self.c.get_lvol_bdev_with_name(self.lvs_name + "/" + snapshot_name2)
        snapshot_bdev = self.c.get_lvol_bdev_with_name(self.lvs_name + "/" + snapshot_name)
        if snapshot_bdev2['driver_specific']['lvol']['base_snapshot'] != snapshot_name:
            fail_count += 1
        if self.lbd_name not in snapshot_bdev2['driver_specific']['lvol']['clones']:
            fail_count += 1
        if snapshot_bdev2['driver_specific']['lvol']['clone'] is not True\
                or snapshot_bdev2['driver_specific']['lvol']['snapshot'] is not True:
            fail_count += 1
        if snapshot_name2 not in snapshot_bdev['driver_specific']['lvol']['clones']:
            fail_count += 1

        # Verify snapshots
        fail_count += self.run_fio_test(nbd_name1, 0, size-1, "read", "0xcc")
        fail_count += self.c.nbd_start_disk(snapshot_bdev2['name'], nbd_name2)
        fail_count += self.run_fio_test(nbd_name2, 0, first_part-1, "read", "0xcc")
        fail_count += self.run_fio_test(nbd_name2, first_part, second_part-first_part, "read", "0xee")
        fail_count += self.run_fio_test(nbd_name2, second_part, size-second_part, "read", "0xcc")

        # Verify lvol bdev
        fail_count += self.run_fio_test(nbd_name0, first_part, second_part-first_part, "read", "0xee")
        fail_count += self.run_fio_test(nbd_name0, second_part, size-second_part, "read", "0xcc")
        lvol_bdev = self.c.get_lvol_bdev_with_name(bdev_name)
        if lvol_bdev['driver_specific']['lvol']['clone'] is not True:
            fail_count += 1
        if lvol_bdev['driver_specific']['lvol']['base_snapshot'] != snapshot_name2:
            fail_count += 1

        # Fill third part of lvol bdev
        fail_count += self.run_fio_test(nbd_name0, second_part, size-second_part, "write", "0xdd")

        # Verify snapshots
        fail_count += self.run_fio_test(nbd_name1, 0, size-1, "read", "0xcc")
        fail_count += self.c.nbd_stop_disk(nbd_name1)
        fail_count += self.run_fio_test(nbd_name2, second_part, size-second_part, "read", "0xcc")
        fail_count += self.c.nbd_stop_disk(nbd_name2)

        # Delete snapshot - should succeed
        fail_count += self.c.bdev_lvol_delete(snapshot_bdev2['name'])

        # Check data consistency
        snapshot_bdev = self.c.get_lvol_bdev_with_name(self.lvs_name + "/" + snapshot_name)
        lvol_bdev = self.c.get_lvol_bdev_with_name(bdev_name)
        if lvol_bdev['driver_specific']['lvol']['clone'] is not True:
            fail_count += 1
        if lvol_bdev['driver_specific']['lvol']['base_snapshot'] != snapshot_name:
            fail_count += 1
        if self.lbd_name not in snapshot_bdev['driver_specific']['lvol']['clones']:
            fail_count += 1
        fail_count += self.run_fio_test(nbd_name0, first_part, second_part-first_part, "read", "0xee")
        fail_count += self.run_fio_test(nbd_name0, second_part, size-second_part, "read", "0xdd")
        fail_count += self.c.nbd_stop_disk(nbd_name0)

        # Destroy snapshot
        fail_count += self.c.bdev_lvol_delete(snapshot_bdev['name'])
        # Destroy lvol bdev
        fail_count += self.c.bdev_lvol_delete(lvol_bdev['name'])

        # Destroy lvol store
        fail_count += self.c.bdev_lvol_delete_lvstore(uuid_store)
        # Delete malloc bdev
        fail_count += self.c.bdev_malloc_delete(base_name)

        # Expected result:
        # - calls successful, return code = 0
        # - no other operation fails
        return fail_count

    @case_message
    def test_case800(self):
        """
        rename_positive

        Positive test for lvol store and lvol bdev rename.
        """
        fail_count = 0

        bdev_uuids = []
        bdev_names = [self.lbd_name + str(i) for i in range(4)]
        bdev_aliases = ["/".join([self.lvs_name, name]) for name in bdev_names]

        # Create malloc bdev
        base_name = self.c.bdev_malloc_create(self.total_size,
                                              self.block_size)
        # Construct lvol store on created malloc bdev
        lvs_uuid = self.c.bdev_lvol_create_lvstore(base_name,
                                                   self.lvs_name)
        fail_count += self.c.check_bdev_lvol_get_lvstores(base_name,
                                                          lvs_uuid,
                                                          self.cluster_size,
                                                          self.lvs_name)
        # Create 4 lvol bdevs on top of previously created lvol store
        bdev_size = self.get_lvs_divided_size(4)
        for name, alias in zip(bdev_names, bdev_aliases):
            uuid = self.c.bdev_lvol_create(lvs_uuid,
                                           name,
                                           bdev_size)
            fail_count += self.c.check_bdev_get_bdevs_methods(uuid,
                                                              bdev_size,
                                                              alias)
            bdev_uuids.append(uuid)

        # Rename lvol store and check if lvol store name and
        # lvol bdev aliases were updated properly
        new_lvs_name = "lvs_new"
        bdev_aliases = [alias.replace(self.lvs_name, new_lvs_name) for alias in bdev_aliases]

        fail_count += self.c.bdev_lvol_rename_lvstore(self.lvs_name, new_lvs_name)

        fail_count += self.c.check_bdev_lvol_get_lvstores(base_name,
                                                          lvs_uuid,
                                                          self.cluster_size,
                                                          new_lvs_name)

        for uuid, alias in zip(bdev_uuids, bdev_aliases):
            fail_count += self.c.check_bdev_get_bdevs_methods(uuid,
                                                              bdev_size,
                                                              alias)

        # Now try to rename the bdevs using their uuid as "old_name"
        # Verify that all bdev names were successfully updated
        bdev_names = ["lbd_new" + str(i) for i in range(4)]
        bdev_aliases = ["/".join([new_lvs_name, name]) for name in bdev_names]
        print(bdev_aliases)
        for uuid, new_name, new_alias in zip(bdev_uuids, bdev_names, bdev_aliases):
            fail_count += self.c.bdev_lvol_rename(uuid, new_name)
            fail_count += self.c.check_bdev_get_bdevs_methods(uuid,
                                                              bdev_size,
                                                              new_alias)
        # Rename lvol bdevs. Use lvols alias name to point which lvol bdev name to change
        # Verify that all bdev names were successfully updated
        bdev_names = ["lbd_even_newer" + str(i) for i in range(4)]
        new_bdev_aliases = ["/".join([new_lvs_name, name]) for name in bdev_names]
        print(bdev_aliases)
        for uuid, old_alias, new_alias, new_name in zip(bdev_uuids, bdev_aliases, new_bdev_aliases, bdev_names):
            fail_count += self.c.bdev_lvol_rename(old_alias, new_name)
            fail_count += self.c.check_bdev_get_bdevs_methods(uuid,
                                                              bdev_size,
                                                              new_alias)

        # Delete configuration using names after rename operation
        for bdev in new_bdev_aliases:
            fail_count += self.c.bdev_lvol_delete(bdev)
        fail_count += self.c.bdev_lvol_delete_lvstore(new_lvs_name)
        fail_count += self.c.bdev_malloc_delete(base_name)

        # Expected results:
        # - lvol store and lvol bdevs correctly created
        # - lvol store and lvol bdevs names updated after renaming operation
        # - lvol store and lvol bdevs possible to delete using new names
        # - no other operation fails
        return fail_count

    @case_message
    def test_case801(self):
        """
        rename_lvs_nonexistent

        Negative test case for lvol store rename.
        Check that error is returned when trying to rename not existing lvol store.
        """
        fail_count = 0
        # Call bdev_lvol_rename_lvstore with name pointing to not existing lvol store
        if self.c.bdev_lvol_rename_lvstore("NOTEXIST", "WHATEVER") == 0:
            fail_count += 1

        # Expected results:
        # - bdev_lvol_rename_lvstore return code != 0
        # - no other operation fails
        return fail_count

    @case_message
    def test_case802(self):
        """
        rename_lvs_EEXIST

        Negative test case for lvol store rename.
        Check that error is returned when trying to rename to a name which is already
        used by another lvol store.
        """
        fail_count = 0

        lvs_name_1 = "lvs_1"
        lvs_name_2 = "lvs_2"

        # Create lists with lvol bdev names and aliases for later use
        bdev_names_1 = ["lvol_1_" + str(i) for i in range(4)]
        bdev_aliases_1 = ["/".join([lvs_name_1, name]) for name in bdev_names_1]
        bdev_uuids_1 = []
        bdev_names_2 = ["lvol_2_" + str(i) for i in range(4)]
        bdev_aliases_2 = ["/".join([lvs_name_2, name]) for name in bdev_names_2]
        bdev_uuids_2 = []

        # Construct two malloc bdevs
        base_bdev_1 = self.c.bdev_malloc_create(self.total_size,
                                                self.block_size)
        base_bdev_2 = self.c.bdev_malloc_create(self.total_size,
                                                self.block_size)

        # Create lvol store on each malloc bdev
        lvs_uuid_1 = self.c.bdev_lvol_create_lvstore(base_bdev_1,
                                                     lvs_name_1)
        fail_count += self.c.check_bdev_lvol_get_lvstores(base_bdev_1,
                                                          lvs_uuid_1,
                                                          self.cluster_size,
                                                          lvs_name_1)
        lvs_uuid_2 = self.c.bdev_lvol_create_lvstore(base_bdev_2,
                                                     lvs_name_2)
        fail_count += self.c.check_bdev_lvol_get_lvstores(base_bdev_2,
                                                          lvs_uuid_2,
                                                          self.cluster_size,
                                                          lvs_name_2)

        # Create 4 lvol bdevs on top of each lvol store
        bdev_size_1 = self.get_lvs_divided_size(4, lvs_name_1)
        bdev_size_2 = self.get_lvs_divided_size(4, lvs_name_2)
        for name, alias in zip(bdev_names_1, bdev_aliases_1):
            uuid = self.c.bdev_lvol_create(lvs_uuid_1,
                                           name,
                                           bdev_size_1)
            fail_count += self.c.check_bdev_get_bdevs_methods(uuid,
                                                              bdev_size_1,
                                                              alias)
            bdev_uuids_1.append(uuid)
        for name, alias in zip(bdev_names_2, bdev_aliases_2):
            uuid = self.c.bdev_lvol_create(lvs_uuid_2,
                                           name,
                                           bdev_size_2)
            fail_count += self.c.check_bdev_get_bdevs_methods(uuid,
                                                              bdev_size_2,
                                                              alias)
            bdev_uuids_2.append(uuid)

        # Call bdev_lvol_rename_lvstore on first lvol store and try to change its name to
        # the same name as used by second lvol store
        if self.c.bdev_lvol_rename_lvstore(lvs_name_1, lvs_name_2) == 0:
            fail_count += 1

        # Verify that names of lvol stores and lvol bdevs did not change
        fail_count += self.c.check_bdev_lvol_get_lvstores(base_bdev_1,
                                                          lvs_uuid_1,
                                                          self.cluster_size,
                                                          lvs_name_1)
        fail_count += self.c.check_bdev_lvol_get_lvstores(base_bdev_2,
                                                          lvs_uuid_2,
                                                          self.cluster_size,
                                                          lvs_name_2)

        for name, alias, uuid in zip(bdev_names_1, bdev_aliases_1, bdev_uuids_1):
            fail_count += self.c.check_bdev_get_bdevs_methods(uuid,
                                                              bdev_size_1,
                                                              alias)

        for name, alias, uuid in zip(bdev_names_2, bdev_aliases_2, bdev_uuids_2):
            fail_count += self.c.check_bdev_get_bdevs_methods(uuid,
                                                              bdev_size_2,
                                                              alias)

        # Clean configuration
        for lvol_uuid in bdev_uuids_1 + bdev_uuids_2:
            fail_count += self.c.bdev_lvol_delete(lvol_uuid)
        fail_count += self.c.bdev_lvol_delete_lvstore(lvs_uuid_1)
        fail_count += self.c.bdev_lvol_delete_lvstore(lvs_uuid_2)
        fail_count += self.c.bdev_malloc_delete(base_bdev_1)
        fail_count += self.c.bdev_malloc_delete(base_bdev_2)

        # Expected results:
        # - bdev_lvol_rename_lvstore return code != 0; not possible to rename to already
        #   used name
        # - no other operation fails

        return fail_count

    @case_message
    def test_case803(self):
        """
        bdev_lvol_rename_nonexistent

        Negative test case for lvol bdev rename.
        Check that error is returned when trying to rename not existing lvol bdev.
        """
        fail_count = 0
        # Call bdev_lvol_rename with name pointing to not existing lvol bdev
        if self.c.bdev_lvol_rename("NOTEXIST", "WHATEVER") == 0:
            fail_count += 1

        # Expected results:
        # - bdev_lvol_rename return code != 0
        # - no other operation fails
        return fail_count

    @case_message
    def test_case804(self):
        """
        bdev_lvol_rename_EEXIST

        Negative test case for lvol bdev rename.
        Check that error is returned when trying to rename to a name which is already
        used by another lvol bdev.
        """
        fail_count = 0

        # Construt malloc bdev
        base_bdev = self.c.bdev_malloc_create(self.total_size,
                                              self.block_size)
        # Create lvol store on created malloc bdev
        lvs_uuid = self.c.bdev_lvol_create_lvstore(base_bdev,
                                                   self.lvs_name)
        fail_count += self.c.check_bdev_lvol_get_lvstores(base_bdev,
                                                          lvs_uuid,
                                                          self.cluster_size,
                                                          self.lvs_name)
        # Construct 2 lvol bdevs on lvol store
        bdev_size = self.get_lvs_divided_size(2)
        bdev_uuid_1 = self.c.bdev_lvol_create(lvs_uuid,
                                              self.lbd_name + "1",
                                              bdev_size)
        fail_count += self.c.check_bdev_get_bdevs_methods(bdev_uuid_1,
                                                          bdev_size)
        bdev_uuid_2 = self.c.bdev_lvol_create(lvs_uuid,
                                              self.lbd_name + "2",
                                              bdev_size)
        fail_count += self.c.check_bdev_get_bdevs_methods(bdev_uuid_2,
                                                          bdev_size)

        # Call bdev_lvol_rename on first lvol bdev and try to change its name to
        # the same name as used by second lvol bdev
        if self.c.bdev_lvol_rename(self.lbd_name + "1", self.lbd_name + "2") == 0:
            fail_count += 1
        # Verify that lvol bdev still have the same names as before
        fail_count += self.c.check_bdev_get_bdevs_methods(bdev_uuid_1,
                                                          bdev_size,
                                                          "/".join([self.lvs_name, self.lbd_name + "1"]))

        fail_count += self.c.bdev_lvol_delete(bdev_uuid_1)
        fail_count += self.c.bdev_lvol_delete(bdev_uuid_2)
        fail_count += self.c.bdev_lvol_delete_lvstore(lvs_uuid)
        fail_count += self.c.bdev_malloc_delete(base_bdev)

        # Expected results:
        # - bdev_lvol_rename return code != 0; not possible to rename to already
        #   used name
        # - no other operation fails
        return fail_count

    @case_message
    def test_case850(self):
        """"
        Clear_method

        Test for clear_method equals to none
        """
        # Create malloc bdev
        base_name = self.c.bdev_malloc_create(self.total_size,
                                              self.block_size)
        # Construct lvol store on created malloc bddev
        lvs_uuid = self.c.bdev_lvol_create_lvstore(base_name,
                                                   self.lvs_name)
        # Check correct uuid values in response bdev_lvol_get_lvstores command
        fail_count = self.c.check_bdev_lvol_get_lvstores(base_name, lvs_uuid,
                                                         self.cluster_size)
        lvs = self.c.bdev_lvol_get_lvstores(self.lvs_name)[0]
        # Construct lvol bdev on lvol store
        lbd_size = int(lvs['cluster_size'] / MEGABYTE)
        bdev_uuid = self.c.bdev_lvol_create(lvs_uuid,
                                            self.lbd_name,
                                            lbd_size,
                                            clear_method='none')
        lvol_bdev = self.c.get_lvol_bdev_with_name(bdev_uuid)

        nbd_name = "/dev/nbd0"
        fail_count += self.c.nbd_start_disk(bdev_uuid, nbd_name)
        # Write pattern to lvol bdev starting from offset 0.
        fail_count += self.run_fio_test(nbd_name, 0, lvs['cluster_size'],
                                        "write", "0xdd")
        fail_count += self.c.nbd_stop_disk(nbd_name)

        # Delete lvol bdev
        fail_count += self.c.bdev_lvol_delete(bdev_uuid)

        # Delete lvol store. We need to do this so that we can attach the underlying malloc
        # bdev to nbd to examine its contents.
        fail_count += self.c.bdev_lvol_delete_lvstore(lvs_uuid)

        fail_count += self.c.nbd_start_disk(base_name, nbd_name)
        metadata_pages = 1 + lvs['total_data_clusters'] + (math.ceil(5 + math.ceil(lvs['total_data_clusters'] / 8) / 4096)) * 3
        last_metadata_lba = int(metadata_pages * 4096 / self.block_size)
        offset_metadata_end = int(last_metadata_lba * self.block_size)
        last_cluster_of_metadata = math.ceil(metadata_pages / lvs['cluster_size'] / 4096)
        offset = last_cluster_of_metadata * lvs['cluster_size']
        size_metadata_end = offset - offset_metadata_end

        # Check if data on area between end of metadata
        # and first cluster of lvol bdev remained unchaged
        fail_count += self.run_fio_test("/dev/nbd0", offset_metadata_end,
                                        size_metadata_end, "read", "0x00")
        # Check if data on first lvol bdevs remains unchanged.
        fail_count += self.run_fio_test("/dev/nbd0", offset, lvs['cluster_size'], "read", "0xdd")
        fail_count += self.c.nbd_stop_disk(nbd_name)

        self.c.bdev_malloc_delete(base_name)

        # Expected result:
        # - calls successful, return code = 0
        # - get_bdevs: no change
        # - no other operation fails
        return fail_count

    @case_message
    def test_case851(self):
        """"
        Clear_method

        Test lvol bdev with clear_method equals to unmap
        """
        # Create malloc bdev
        base_name = self.c.bdev_malloc_create(self.total_size,
                                              self.block_size)

        nbd_name = "/dev/nbd0"
        fail_count = self.c.nbd_start_disk(base_name, nbd_name)

        # Write data to malloc bdev starting from offset 0.
        fail_count += self.run_fio_test(nbd_name, 0, self.total_size * MEGABYTE,
                                        "write", "0xdd")
        fail_count += self.c.nbd_stop_disk(nbd_name)

        # Construct lvol store on created malloc bddev
        lvs_uuid = self.c.bdev_lvol_create_lvstore(base_name,
                                                   self.lvs_name,
                                                   clear_method='none')
        # Check correct uuid values in response bdev_lvol_get_lvstores command
        fail_count = self.c.check_bdev_lvol_get_lvstores(base_name, lvs_uuid,
                                                         self.cluster_size)
        lvs = self.c.bdev_lvol_get_lvstores(self.lvs_name)[0]

        # Construct lvol bdev on lvol store
        lbd_size = int(lvs['cluster_size'] / MEGABYTE)
        bdev_uuid = self.c.bdev_lvol_create(lvs_uuid,
                                            self.lbd_name,
                                            lbd_size,
                                            clear_method='unmap')
        # Check that data on lvol bdev remains unchanged
        fail_count += self.c.nbd_start_disk(bdev_uuid, nbd_name)
        fail_count += self.run_fio_test(nbd_name, 0, lvs['cluster_size'],
                                        "read", "0xdd")
        fail_count += self.c.nbd_stop_disk(nbd_name)

        # Delete lvol bdev
        fail_count += self.c.bdev_lvol_delete(bdev_uuid)

        # Delete lvol store
        fail_count += self.c.bdev_lvol_delete_lvstore(lvs_uuid)

        fail_count += self.c.nbd_start_disk(base_name, nbd_name)
        metadata_pages = 1 + lvs['total_data_clusters'] + \
            (math.ceil(5 + math.ceil(lvs['total_data_clusters'] / 8) / 4096)) * 3
        last_metadata_lba = int(metadata_pages * 4096 / self.block_size)
        offset_metadata_end = int(last_metadata_lba * self.block_size)
        last_cluster_of_metadata = math.ceil(metadata_pages / lvs['cluster_size'] / 4096)
        offset = last_cluster_of_metadata * lvs['cluster_size']
        size_metadata_end = offset - offset_metadata_end

        # Check if data on area between end of metadata
        # and first cluster of lvol bdev remained unchaged
        fail_count += self.run_fio_test("/dev/nbd0", offset_metadata_end,
                                        size_metadata_end, "read", "0xdd")
        # Check if data on lvol bdev was zeroed.
        # Malloc bdev should zero any data that is unmapped.
        fail_count += self.run_fio_test("/dev/nbd0", offset, lvs['cluster_size'], "read", "0x00")

        self.c.bdev_malloc_delete(base_name)

        # Expected result:
        # - calls successful, return code = 0
        # - get_bdevs: no change
        # - no other operation fails
        return fail_count

    @case_message
    def test_case10000(self):
        """
        SIGTERM

        Call CTRL+C (SIGTERM) occurs after creating lvol store
        """
        pid_path = path.join(self.path, 'vhost.pid')
        # Create malloc bdev
        base_name = self.c.bdev_malloc_create(self.total_size,
                                              self.block_size)
        # Construct lvol store on created malloc bddev
        uuid_store = self.c.bdev_lvol_create_lvstore(base_name,
                                                     self.lvs_name)
        # Check correct uuid values in response bdev_lvol_get_lvstores command
        fail_count = self.c.check_bdev_lvol_get_lvstores(base_name, uuid_store,
                                                         self.cluster_size)

        # Send SIGTERM signal to the application
        fail_count += self._stop_vhost(pid_path)

        # Expected result:
        # - calls successful, return code = 0
        # - bdev_get_bdevs: no change
        # - no other operation fails
        return fail_count
