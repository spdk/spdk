#!/usr/bin/env python
import io
import sys
import random
import signal
import subprocess
import pprint
import socket

from errno import ESRCH
from os import kill, path, unlink, path, listdir, remove
from nbd_rpc_commands_lib import Commands_Rpc
from time import sleep
#from uuid import uuid4


def test_counter():
    '''
    :return: the number of tests
    '''
    return 6


def header(num):
    test_name = {
        1: 'start_nbd_disk_positive',
        2: 'list_nbd_disks_one_by_one_positive',
        3: 'list_all_nbd_disks_positive',
        4: 'stop_nbd_disk_positive',
        5: 'lsblk_check_positive',
        6: 'write_workload_by_dd_positive',
    }
    print("========================================================")
    print("Test Case {num}: Start".format(num=num))
    print("Test Name: {name}".format(name=test_name[num]))
    print("========================================================")

def footer(num):
    print("Test Case {num}: END\n".format(num=num))
    print("========================================================")

class TestCases(object):

    def __init__(self, rpc_py, pid_file, app_file, conf_file):
        self.c = Commands_Rpc(rpc_py)
        self.pid_file = pid_file
        self.app_file = app_file
        self.conf_file = conf_file

    def _stop_nbd_app(self):
        with io.open(self.pid_file, 'r') as nbd_pid:
            pid = int(nbd_pid.readline())
            if pid:
                try:
                    kill(pid, signal.SIGTERM)
                    for count in range(30):
                        print("Info: Terminating nbd app...")
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

    def _start_nbd_app(self):
        ### start nbd app and record its pid
        cmd = "{app} -c {config} & pid=$!; echo $pid > {pidf}".format(
            app=self.app_file, config=self.conf_file, pidf=self.pid_file)
        subprocess.call(cmd, shell=True)

        ### wait for listen
        sock = socket.socket(socket.AF_UNIX)
        for timeo in range(30):
            if timeo == 29:
                print("ERROR: Timeout on waitting for app start")
                return 1
            try:
                sock.connect("/var/tmp/spdk.sock")
            except socket.error as e:
                print("Info: Waitting for RPC Unix socket...")
                sleep(1)
                continue
            else:
                sock.close()
                break
        return 0

    # positive tests
    def test_case1(self):
        header(1)
        unclaimed_bdevs = self.c.get_unclaimed_bdevs()
        unused_nbds = self.c.get_unused_nbds()
        spdk_nbds, fail_count = self.c.start_nbd_disks(unclaimed_bdevs, unused_nbds)
        ### restart nbd app to keep a clear configuration
        fail_count += self._stop_nbd_app()
        remove(self.pid_file)
        if self._start_nbd_app() != 0:
            fail_count += 1
        footer(1)
        return fail_count

    def test_case2(self):
        header(2)
        unclaimed_bdevs = self.c.get_unclaimed_bdevs()
        unused_nbds = self.c.get_unused_nbds()
        spdk_nbds, fail_count = self.c.start_nbd_disks(unclaimed_bdevs, unused_nbds)
        fail_count += self.c.get_nbd_disks(spdk_nbds)
        ### restart nbd app to keep a clear configuration
        fail_count += self._stop_nbd_app()
        remove(self.pid_file)
        if self._start_nbd_app() != 0:
            fail_count += 1
        footer(2)
        return fail_count

    def test_case3(self):
        header(3)
        unclaimed_bdevs = self.c.get_unclaimed_bdevs()
        unused_nbds = self.c.get_unused_nbds()
        spdk_nbds, fail_count = self.c.start_nbd_disks(unclaimed_bdevs, unused_nbds)
        fail_count += self.c.get_nbd_disks([])
        ### restart nbd app to keep a clear configuration
        fail_count += self._stop_nbd_app()
        remove(self.pid_file)
        if self._start_nbd_app() != 0:
            fail_count += 1
        footer(3)
        return fail_count

    def test_case4(self):
        header(4)
        unclaimed_bdevs = self.c.get_unclaimed_bdevs()
        unused_nbds = self.c.get_unused_nbds()
        spdk_nbds, fail_count = self.c.start_nbd_disks(unclaimed_bdevs, unused_nbds)
        fail_count += self.c.stop_nbd_disks(spdk_nbds)
        ### restart nbd app to keep a clear configuration
        fail_count += self._stop_nbd_app()
        remove(self.pid_file)
        if self._start_nbd_app() != 0:
            fail_count += 1
        footer(4)
        return fail_count

    def test_case5(self):
        header(5)
        unclaimed_bdevs = self.c.get_unclaimed_bdevs()
        unused_nbds = self.c.get_unused_nbds()
        spdk_nbds, fail_count = self.c.start_nbd_disks(unclaimed_bdevs, unused_nbds)
        ### lsblk will read head of each block dev
        unused_nbds_post = self.c.get_unused_nbds()
        if len(set(unused_nbds) - set(spdk_nbds) - set(unused_nbds_post)) != 0:
            print("ERROR: failed on lsblk check")
            fail_count += 1
        ### restart nbd app to keep a clear configuration
        fail_count += self._stop_nbd_app()
        remove(self.pid_file)
        if self._start_nbd_app() != 0:
            fail_count += 1
        footer(5)
        return fail_count

    def test_case6(self):
        header(6)
        unclaimed_bdevs = self.c.get_unclaimed_bdevs()
        unused_nbds = self.c.get_unused_nbds()
        spdk_nbds, fail_count = self.c.start_nbd_disks(unclaimed_bdevs, unused_nbds)
        ### Record 1MB rand data into tmp file by dd
        base_path = path.dirname(sys.argv[0])
        tmp_dd = path.join(base_path, 'tmd_dd_file')
        cmd = "dd if=/dev/urandom of={tmpf} bs=4k count=256".format(tmpf=tmp_dd)
        print(cmd)
        dd_proc = subprocess.Popen(cmd, shell=True)
        dd_proc.wait()
        ### Write that 1MB rand data into each nbd device by dd
        for i in range(len(spdk_nbds)):
            cmd = "dd if={tmpf} of={nbd} bs=4k count=256".format(
                tmpf=tmp_dd, nbd=spdk_nbds[i])
            print(cmd)
            dd_proc = subprocess.Popen(cmd, shell=True)
            dd_proc.wait()
        ### Compare writen data
        for i in range(len(spdk_nbds)):
            cmd = "cmp {nbd} {tmpf} -n 1M".format(
                tmpf=tmp_dd, nbd=spdk_nbds[i])
            print(cmd)
            fail_count += subprocess.call(cmd, shell=True)
        remove(tmp_dd)
        ### restart nbd app to keep a clear configuration
        fail_count += self._stop_nbd_app()
        remove(self.pid_file)
        if self._start_nbd_app() != 0:
            fail_count += 1
        footer(6)
        return fail_count
