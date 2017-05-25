#!/usr/bin/env python
# BSD LICENSE
#
# Copyright(c) 2010-2014 Intel Corporation. All rights reserved.
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
#
#   * Redistributions of source code must retain the above copyright
#     notice, this list of conditions and the following disclaimer.
#   * Redistributions in binary form must reproduce the above copyright
#     notice, this list of conditions and the following disclaimer in
#     the documentation and/or other materials provided with the
#     distribution.
#   * Neither the name of Intel Corporation nor the names of its
#     contributors may be used to endorse or promote products derived
#     from this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

import time
import datetime
import os
import sys
from test_case import TestCase
from Filesystem_integrity_test import FileSystem_integrity


class TestFileSystemIntegrity(object):

    def set_up_all(self, test_case_obj, backendname):
        """
        Run at the start of each test suite.
        fio Prerequisites
        """
        if self.nic == "ConnectX4":
            self.tester.send_expect("modprobe mlx5_ib", "#", 5)
        if self.nic == "ConnectX3":
            self.tester.send_expect("modprobe mlx4_ib", "#", 5)
        if self.nic == "chelsio_40gb":
            self.tester.send_expect("modprobe iw_cxgb4", "#", 5)
        self.backend = backendname
        self.dut_ports = []
        self.tester_ports = []
        self.dut_ports_all = self.dut.get_ports()
        self.tester_ports_all = self.tester.get_ports()
        self.is_port = self._get_nic_driver(self.nic)
        for i, self.dut_port in enumerate(self.dut_ports_all[1]):
            if self.dut_port == self.is_port + '\r':
                self.dut_port_nic = self.dut_ports_all[0][i]
                self.dut_ports.append(self.dut_port_nic)
        for j, self.tester_port in enumerate(self.tester_ports_all[1]):
            if self.tester_port == self.is_port + '\r':
                self.tester_port_nic = self.tester_ports_all[0][j]
                self.tester_ports.append(self.tester_port_nic)
        self.verify(len(self.dut_ports) >= 1, "Insufficient ports")
        self.dut_port_0_inf = self.dut_ports[0]
        self.tester_port_0_inf = self.tester_ports[0]

        self.dut.send_expect("cd %s" % self.dut.base_dir, "# ", 5)
        self.initial_real_path = self.dut.base_dir
        test_suite_path = os.getcwd() + "/../tests"
        self.tester_utils_path = "%s/lib/" % test_suite_path
        self.tester_utils_file = self.tester_utils_path + "Test_base_utils.py"
        self.dut_utils_path = self.initial_real_path + "/etc/spdk"
        self.nvmf_config_path = self.initial_real_path + "/etc/spdk/nvmf.conf.in"
        self.iscsi_config_path = self.initial_real_path + "/etc/spdk/iscsi.conf.in"
        self.copy_file_to_dut(self.tester_utils_file, self.dut_utils_path)
        self.dut.send_expect('sed -i "s/  AIO/#  AIO/" %s' %
                             self.nvmf_config_path, "# ", 10)
        self.dut.send_expect(
            'sed -i "s/#MaxQueueDepth 128/MaxQueueDepth 1024/" %s' % self.nvmf_config_path, "# ", 10)
        self.dut.send_expect(
            'sed -i "s/#MaxIOSize 131072/MaxIOSize 131072/" %s' % self.nvmf_config_path, "# ", 10)
        self.dut.send_expect(
            'sed -i "s/TransportId/#TransportId/" %s' % self.nvmf_config_path, "# ", 10)
        self.dut.send_expect(
            'sed -i "s/RetryCount 4/#RetryCount 4/" %s' % self.nvmf_config_path, "# ", 10)
        self.dut.send_expect(
            "sed -i 's/192.168.2.21/192.168.1.11/' %s" % self.iscsi_config_path, "# ", 10)
        self.dut.send_expect(
            "sed -i 's/192.168.2.0/192.168.1.0/' %s" % self.iscsi_config_path, "# ", 10)

    def copy_file_to_dut(self, file_in_tester, dut_file_path):
        self.dut.session.copy_file_to(file_in_tester)
        file_name = file_in_tester.split('/')[-1]
        self.dut.send_expect("mv -f /root/%s %s" %
                             (file_name, dut_file_path), "# ", 5)

    def kill_target(self):
        """
        Kill target when finish one test case
        """
        if self.backend == "nvme_direct":
            self.tester.send_expect("umount /home/devicedev0*", "# ", 10)
            self.tester.send_expect("rm -rf /home/devicedev0*", "# ", 10)
            self.tester.send_expect(
                'nvme disconnect -n "nqn.2016-06.io.spdk:cnode1"', "# ")
            self.tester.send_expect(
                'nvme disconnect -n "nqn.2016-06.io.spdk:cnode2"', "# ")
            self.tester.send_expect(
                'nvme disconnect -n "nqn.2016-06.io.spdk:cnode3"', "# ")
            self.tester.send_expect(
                'nvme disconnect -n "nqn.2016-06.io.spdk:cnode4"', "# ")
            out = self.dut.alt_session.send_expect(
                "ps aux | grep nvmf_tgt | awk '{print $2}'", "# ", 10)
            self.dut.send_expect("kill -9 %s" % out.splitlines()[0], "# ", 10)
            time.sleep(3)
        if self.backend == "iscsi_nvme":
            self.tester.send_expect("umount /home/devicedev0*", "# ", 10)
            self.tester.send_expect("rm -rf /home/devicedev0*", "# ", 10)
            self.tester.send_expect("iscsiadm -m node --logout", "# ")
            self.tester.send_expect("iscsiadm -m node -o delete", "# ")
            out = self.dut.alt_session.send_expect(
                "ps aux | grep iscsi_tgt | awk '{print $2}'", "# ", 10)
            self.dut.send_expect("kill -9 %s" % out.splitlines()[0], "# ", 10)
            time.sleep(3)

    def set_up(self):
        """
        Run before each test case.
        """
        if self.backend == "nvme_direct":
            self.tester.send_expect(
                "ifconfig %s 192.168.3.2" %
                self.tester_port_0_inf, "# ", 5)
            self.dut.send_expect(
                "ifconfig %s 192.168.3.11" %
                self.dut_port_0_inf, "# ", 5)
            self.dut.send_expect(
                "rm -rf nvmf.conf && cp etc/spdk/nvmf.conf.in nvmf.conf ", "# ", 200)
            self.dut.send_expect(
                "python etc/spdk/Test_base_utils.py generate_nvmf_tgt_file nvme_direct nvmf.conf",
                "# ",
                200)
            self.dut.send_expect("NRHUGE=12288 ./scripts/setup.sh", "#", 200)
            self.dut.send_expect(
                "ps -ef|grep nvmf_tgt|grep -v grep|awk '{print $2}'|xargs kill -9 & ",
                "# ",
                200)
            time.sleep(2)
            self.dut.send_expect(
                "./app/nvmf_tgt/nvmf_tgt -c nvmf.conf &", "# ", 200)
            time.sleep(30)
            print "Waiting for nvmf target to connect..."
            time.sleep(2)
            self.tester.send_expect("modprobe nvme-rdma", "# ", 10)
            time.sleep(2)
            self.tester.send_expect("modprobe nvme-fabrics", "# ", 10)
            time.sleep(2)
            self.tester.send_expect(
                "nvme discover -t rdma -a 192.168.3.11 -s 4420", "# ", 10)
            time.sleep(10)
            self.tester.send_expect(
                'nvme connect -t rdma -n "nqn.2016-06.io.spdk:cnode1" -a 192.168.3.11 -s 4420',
                "# ",
                10,
                verify=True)
            self.tester.send_expect(
                'nvme connect -t rdma -n "nqn.2016-06.io.spdk:cnode2" -a 192.168.3.11 -s 4420',
                "# ",
                10,
                verify=True)
            self.tester.send_expect(
                'nvme connect -t rdma -n "nqn.2016-06.io.spdk:cnode3" -a 192.168.3.11 -s 4420',
                "# ",
                10,
                verify=True)
            self.tester.send_expect(
                'nvme connect -t rdma -n "nqn.2016-06.io.spdk:cnode4" -a 192.168.3.11 -s 4420',
                "# ",
                10,
                verify=True)
        if self.backend == "iscsi_nvme":
            self.tester.send_expect(
                "ifconfig %s 192.168.1.10" %
                self.tester_port_0_inf, "# ", 5)
            self.dut.send_expect(
                "ifconfig %s 192.168.1.11" %
                self.dut_port_0_inf, "# ", 5)
            self.dut.send_expect(
                "rm -rf iscsi.conf && cp etc/spdk/iscsi.conf.in iscsi.conf ", "# ", 200)
            self.dut.send_expect(
                "python etc/spdk/Test_base_utils.py generate_iscsi_file iscsi_nvme iscsi.conf ",
                "# ",
                200)
            self.dut.send_expect("NRHUGE=12288 ./scripts/setup.sh", "#", 200)
            self.dut.send_expect(
                "./app/iscsi_tgt/iscsi_tgt -c iscsi.conf 2>&1 & ", "# ", 200)
            time.sleep(30)
            self.tester.send_expect(
                "iscsiadm -m discovery -t st -p 192.168.1.11", "# ", 10)
            time.sleep(10)
            self.tester.send_expect(
                "iscsiadm -m node --login", "# ", 10, verify=True)

    def test_ext4_large_file(self):
        start_time = datetime.datetime.now()
        ext4test = FileSystem_integrity('ext4', self.backend)
        # change 30 for daily testing
        # change 120 for weekend testing
        out = ext4test.run_filesystem_integrity(30, 'large')
        end_time = datetime.datetime.now()
        duration = end_time - start_time
        print "duration of ext4_large_file is {0}".format(duration)
        self.verify("All tests passed" in out,
                    "test ext4 large file system failed")

    def test_ext4_small_file(self):
        start_time = datetime.datetime.now()
        ext4test = FileSystem_integrity('ext4', self.backend)
        # change 400 for daily testing
        # change 1200 for weekend testing
        out = ext4test.run_filesystem_integrity(400, 'small')
        end_time = datetime.datetime.now()
        duration = end_time - start_time
        print "duration of ext4_small_file is {0}".format(duration)
        self.verify("All tests passed" in out,
                    "test ext4 small file system failed")

    def test_ext4_compile_kernel(self):
        start_time = datetime.datetime.now()
        ext4test = FileSystem_integrity('ext4', self.backend)
        # change 20 for daily testing
        # change 120 for weekend testing
        out = ext4test.onlycompilekernel(20)
        end_time = datetime.datetime.now()
        duration = end_time - start_time
        print "duration of ext4_compile_kernel is {0}".format(duration)
        self.verify("All tests passed" in out,
                    "test ext4 compile kernel failed")

    def test_btrfs_large_file(self):
        start_time = datetime.datetime.now()
        btrfstest = FileSystem_integrity('btrfs', self.backend)
        # change 30 for daily testing
        # change 120 for weekend testing
        out = btrfstest.run_filesystem_integrity(30, 'large')
        end_time = datetime.datetime.now()
        duration = end_time - start_time
        print "duration of btrfs_large_file is {0}".format(duration)
        self.verify("All tests passed" in out,
                    "test btrfs large  file system failed")

    def test_btrfs_small_file(self):
        start_time = datetime.datetime.now()
        btrfstest = FileSystem_integrity('btrfs', self.backend)
        # change 400 for daily testing
        # change 1200 for weekend testing
        out = btrfstest.run_filesystem_integrity(400, 'small')
        end_time = datetime.datetime.now()
        duration = end_time - start_time
        print "duration of btrfs_small_file is {0}".format(duration)
        self.verify("All tests passed" in out,
                    "test btrfs small file system failed")

    def test_btrfs_compile_kernel(self):
        start_time = datetime.datetime.now()
        btrfstest = FileSystem_integrity('btrfs', self.backend)
        # change 20 for daily testing
        # change 120 for weekend testing
        out = btrfstest.onlycompilekernel(20)
        end_time = datetime.datetime.now()
        duration = end_time - start_time
        print "duration of btrfs_compile_kernel is {0}".format(duration)
        self.verify("All tests passed" in out,
                    "test btrfs compile kernel failed")

    def test_xfs_large_file(self):
        start_time = datetime.datetime.now()
        xfstest = FileSystem_integrity('xfs', self.backend)
        # change 15 for daily testing
        # change 50 for weekend testing
        out = xfstest.run_filesystem_integrity(15, 'large')
        end_time = datetime.datetime.now()
        duration = end_time - start_time
        print "duration of xfs_large_file is {0}".format(duration)
        self.verify("All tests passed" in out,
                    "test xfs large  file system failed")

    def test_xfs_small_file(self):
        start_time = datetime.datetime.now()
        xfstest = FileSystem_integrity('xfs', self.backend)
        # change 400 for daily testing
        # change 1200 for weekend testing
        out = xfstest.run_filesystem_integrity(400, 'small')
        end_time = datetime.datetime.now()
        duration = end_time - start_time
        print "duration of xfs_small_file is {0}".format(duration)
        self.verify("All tests passed" in out,
                    "test xfs small file system failed")

    def test_xfs_compile_kernel(self):
        start_time = datetime.datetime.now()
        xfstest = FileSystem_integrity('xfs', self.backend)
        # change 20 for daily testing
        # change 120 for weekend testing
        out = xfstest.onlycompilekernel(20)
        end_time = datetime.datetime.now()
        duration = end_time - start_time
        print "duration of xfs_compile_kernel is {0}".format(duration)
        self.verify("All tests passed" in out,
                    "test xfs compile kernel failed")

    def tear_down(self):
        """
        Run after each test case.
        """
        print 'tear down'
        self.kill_target()
