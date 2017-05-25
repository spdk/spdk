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
import os
import sys
import pdb
import re
from test_case import TestCase
from exception import VerifyFailure
from Test_base_utils import write_fio_config, generate_nvmf_tgt_file
import Fio_test
import Fio_iscsi_test

iscsibackend = [
    "iscsi_aiobackend",
    "iscsi_malloc",
    "iscsi_nvme",
    "iscsi_multiconnection",
    "iscsi_rxtxqueue"]
nvmfbackend = [
    "nvmf_aiobackend",
    "nvmf_malloc",
    "nvme_virtual",
    "nvmf_multiconnection",
    "nvme_direct"]


class TestFio(object):

    def set_up_all(self, test_case_obj, backendname,
                   all_run_size, all_run_time):
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
        self.DEFAULT_RUN_SIZE = all_run_size
        self.DEFAULT_RUN_TIME = all_run_time
        self.tester_ports = []
        self.dut_ports = []
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
        self.dut_port_1_inf = self.dut_ports[1]
        self.tester_port_0_inf = self.tester_ports[0]
        self.tester_port_1_inf = self.tester_ports[1]
        self.dut_ips = {'net_seg_1': "192.168.1.11",
                        'net_seg_2': "192.168.2.11",
                        'net_seg_3': "192.168.3.11",
                        'net_seg_4': "192.168.4.11"}
        self.tester_ips = {'net_seg_1': "192.168.1.10",
                           'net_seg_2': "192.168.2.10",
                           'net_seg_3': "192.168.3.2",
                           'net_seg_4': "192.168.4.2"}
        self.dut.send_expect("cd %s " % self.dut.base_dir, "# ", 5)
        self.initial_real_path = self.dut.base_dir
        self.dut_utils_path = self.initial_real_path + "/etc/spdk"
        self.dut_iscsi_config_path = self.initial_real_path + "/etc/spdk/iscsi.conf.in"
        self.dut_nvmf_config_path = self.initial_real_path + "/etc/spdk/nvmf.conf.in"
        self.dut_fiotest_path = self.dut_utils_path
        test_suite_path = os.getcwd() + "/../tests"
        self.tester_fiotest_path = "%s/lib/" % test_suite_path
        self.tester_fiotest_conf = self.tester_fiotest_path + "Fio_test.conf"
        self.tester_fiotest_client = self.tester_fiotest_path + "Fio_test.py"
        self.tester_utils_path = "%s/lib/" % test_suite_path
        self.tester_utils_file = self.tester_utils_path + "Test_base_utils.py"
        self.copy_file_to_dut(self.tester_utils_file, self.dut_utils_path)
        if self.backend != "nvmf_aiobackend":
            self.dut.send_expect(
                'sed -i "s/  AIO/#  AIO/" %s' %
                self.dut_nvmf_config_path, "# ", 10)
            self.dut.send_expect(
                'sed -i "s#/dev/sdb#/dev/device1#" %s' %
                self.dut_nvmf_config_path, "# ", 10)
            self.dut.send_expect(
                'sed -i "s#/dev/sdc#/dev/device2#" %s' %
                self.dut_nvmf_config_path, "# ", 10)
        else:
            # self.dut.send_expect('sed -i "s#/dev/sdb#/dev/aio_device1#" %s' % self.dut_nvmf_config_path, "# ", 10)
            # self.dut.send_expect('sed -i "s#/dev/sdc#/dev/aio_device2#" %s' % self.dut_nvmf_config_path, "# ", 10)
            nvmf_path = os.path.dirname(os.path.dirname(__file__))
            path = nvmf_path + '/lib/Test_base_fio.py'
            path_file = open(path, "r")
            is_aiobackend = re.findall(
                r"\n+            # self.dut.send_expect(.*)",
                path_file.read())
            if is_aiobackend[
                    0] == '(\'sed -i "s#/dev/sdb#/dev/aio_device1#" %s\' % self.dut_nvmf_config_path, "# ", 10)':
                path1 = nvmf_path + \
                    "/lib/Test_base_fio.py:125: E265 block comment should start with '# ' "
                print "Please modify aio_device1, The path is", path1
                self.verify(False, "Not setting target backend!!!")
            if is_aiobackend[
                    1] == '(\'sed -i "s#/dev/sdc#/dev/aio_device2#" %s\' % self.dut_nvmf_config_path, "# ", 10)':
                path2 = nvmf_path + \
                    "/lib/Test_base_fio.py:126: E265 block comment should start with '# ' "
                print "Please modify aio_device2, The path is", path2
                self.verify(False, "Not setting target backend!!!")
        if self.backend == "nvmf_multiconnection":
            self.dut.send_expect(
                'sed -i "s/NumberOfLuns 8/NumberOfLuns 128/" %s' %
                self.dut_nvmf_config_path, "# ", 10)
            self.dut.send_expect(
                'sed -i "s/LunSizeInMB 64/LunSizeInMB 1/" %s' %
                self.dut_nvmf_config_path, "# ", 10)
            self.dut.send_expect(
                'sed -i "s/Split Malloc2 2/#Split Malloc2 2/" %s' %
                self.dut_nvmf_config_path, "# ", 10)
            self.dut.send_expect(
                'sed -i "s/Split Malloc3 8 1/#Split Malloc3 8 1/" %s' %
                self.dut_nvmf_config_path, "# ", 10)
        self.dut.send_expect(
            'sed -i "s/#MaxQueueDepth 128/MaxQueueDepth 1024/" %s' %
            self.dut_nvmf_config_path, "# ", 10)
        self.dut.send_expect(
            'sed -i "s/#MaxIOSize 131072/MaxIOSize 131072/" %s' %
            self.dut_nvmf_config_path, "# ", 10)
        self.dut.send_expect(
            'sed -i "s/TransportId/#TransportId/" %s' %
            self.dut_nvmf_config_path, "# ", 10)
        self.dut.send_expect(
            'sed -i "s/RetryCount 4/#RetryCount 4/" %s' %
            self.dut_nvmf_config_path, "# ", 10)
        self.dut.send_expect(
            "sed -i 's/192.168.2.21/192.168.1.11/' %s" %
            self.dut_iscsi_config_path, "# ", 10)
        self.dut.send_expect(
            "sed -i 's/192.168.2.0/192.168.1.0/' %s" %
            self.dut_iscsi_config_path, "# ", 10)
        if self.backend == "iscsi_multiconnection" or "iscsi_rxtxqueue":
            self.dut.send_expect(
                "sed -i 's/#ReactorMask 0xFFFF/ReactorMask 0xFFFF/' %s" %
                self.dut_iscsi_config_path, "# ", 10)
        self.write_fio_target()

    def set_up(self):
        """
        Run before each test case.
        """
        if self.backend in iscsibackend:
            self.tester.send_expect("ifconfig %s %s" % (self.tester_port_0_inf,
                                                        self.tester_ips['net_seg_1']), "# ", 5)
            self.tester.send_expect("ifconfig %s %s" % (self.tester_port_1_inf,
                                                        self.tester_ips['net_seg_2']), "# ", 5)
            self.dut.send_expect("ifconfig %s %s" % (self.dut_port_0_inf,
                                                     self.dut_ips['net_seg_1']), "# ", 5)
            self.dut.send_expect("ifconfig %s %s" % (self.dut_port_1_inf,
                                                     self.dut_ips['net_seg_2']), "# ", 5)
            self.create_iscsi_config()
            self.dut.send_expect(
                "ps -ef|grep iscsi_tgt|grep -v grep|awk '{print $2}'|xargs kill -9 & ",
                "# ",
                200)
            time.sleep(2)
            self.dut.send_expect("NRHUGE=12288 ./scripts/setup.sh", "#", 200)
            self.dut.send_expect(
                "./app/iscsi_tgt/iscsi_tgt -c iscsi.conf &", "# ", 200)
            time.sleep(30)
            self.tester.send_expect(
                "iscsiadm -m discovery -t st -p 192.168.1.11", "# ", 10)
            self.tester.send_expect("iscsiadm -m node --login", "# ", 10)
        if self.backend in nvmfbackend:
            self.tester.send_expect(
                "ifconfig %s %s" %
                (self.tester_port_0_inf, self.tester_ips['net_seg_3']), "# ", 5)
            self.tester.send_expect(
                "ifconfig %s %s" %
                (self.tester_port_1_inf, self.tester_ips['net_seg_4']), "# ", 5)
            self.dut.send_expect(
                "ifconfig %s %s" %
                (self.dut_port_0_inf, self.dut_ips['net_seg_3']), "# ", 5)
            self.dut.send_expect(
                "ifconfig %s %s" %
                (self.dut_port_1_inf, self.dut_ips['net_seg_4']), "# ", 5)
            self.create_nvmf_tgt_config()
            self.dut.send_expect(
                "ps -ef|grep nvmf_tgt|grep -v grep|awk '{print $2}'|xargs kill -9 & ",
                "# ",
                200)
            time.sleep(2)
            self.dut.send_expect("NRHUGE=12288 ./scripts/setup.sh", "#", 200)
            self.dut.send_expect(
                "./app/nvmf_tgt/nvmf_tgt -c nvmf.conf & ", "# ", 200)
            time.sleep(30)
            print "Waiting for connecting nvmf target..."
            self.dut.send_expect("modprobe nvme-rdma", "# ", 5)
            self.dut.send_expect("modprobe nvme-fabrics", "# ", 5)
            self.dut.send_expect(
                "nvme discover -t rdma -a 192.168.3.11 -s 4420", "# ", 5)
            self.tester.send_expect(
                'nvme connect -t rdma -n "nqn.2016-06.io.spdk:cnode1" -a 192.168.3.11 -s 4420',
                "# ",
                5)
            self.tester.send_expect(
                'nvme connect -t rdma -n "nqn.2016-06.io.spdk:cnode2" -a 192.168.3.11 -s 4420',
                "# ",
                5)
            if self.backend == "nvmf_malloc":
                number = 6
                for i in range(number):
                    n = i + 3
                    self.tester.send_expect(
                        'nvme connect -t rdma -n "nqn.2016-06.io.spdk:cnode{}" -a 192.168.3.11 -s 4420'.format(
                            n),
                        "# ",
                        5)
            if self.backend == "nvmf_multiconnection":
                number = 126
                for i in range(number):
                    n = i + 3
                    self.tester.send_expect(
                        'nvme connect -t rdma -n "nqn.2016-06.io.spdk:cnode{}" -a 192.168.3.11 -s 4420'.format(
                            n),
                        "# ",
                        5)

    def create_nvmf_tgt_config(self):
        self.dut.send_expect(
            "rm -rf nvmf.conf && cp etc/spdk/nvmf.conf.in nvmf.conf ", "# ", 200)
        self.dut.send_expect(
            "python etc/spdk/Test_base_utils.py generate_nvmf_tgt_file %s nvmf.conf " %
            self.backend, "# ", 200)

    def create_iscsi_config(self):
        self.dut.send_expect(
            "rm -rf iscsi.conf && cp etc/spdk/iscsi.conf.in iscsi.conf ", "# ", 200)
        self.dut.send_expect(
            "python etc/spdk/Test_base_utils.py generate_iscsi_file %s iscsi.conf " % self.backend, "# ", 200)

    def write_fio_target(self):
        target_options = {'iscsi_addr': self.dut_ips['net_seg_1'],
                          'iscsi_testserver_addr': 'http://' + self.dut_ips['net_seg_2'] + ':8000',
                          'nvmf_addr': self.dut_ips['net_seg_3'],
                          'nvmf_testserver_addr': 'http://' + self.dut_ips['net_seg_4'] + ':8000'}
        write_fio_config(self.tester_fiotest_conf, 'target', **target_options)

    def write_fio_test(self, test_type, io_size, queue_depth,
                       runtime, runsize, verify, rwmixread):
        test_options = {'fio_path': '/usr/bin/fio',
                        'test_types': test_type,
                        'io_size': io_size,
                        'queue_depth': queue_depth,
                        'runsize': runsize,
                        'runtime': runtime,
                        'verify': verify,
                        'rwmixread': rwmixread
                        }
        write_fio_config(self.tester_fiotest_conf, 'test', **test_options)

    def copy_file_to_dut(self, file_in_tester, dut_file_path):
        self.dut.session.copy_file_to(file_in_tester)
        file_name = file_in_tester.split('/')[-1]
        self.dut.send_expect("mv -f /root/%s %s" %
                             (file_name, dut_file_path), "# ", 5)

    def kill_dut_process(self, process):
        command = "ps aux | grep {0} | grep -v grep | awk '{{print $2}}'".format(
            process)
        out = self.dut.alt_session.send_expect(command, "# ", 10)
        if not out:
            print "There is no process [ {0} ] in dut!!!".format(process)
        else:
            self.dut.alt_session.send_expect(
                "kill -9 %s" % out.splitlines()[0], "# ", 10)
            time.sleep(2)
            out = self.dut.alt_session.send_expect(command, "# ", 10)
            if out:
                print "kill dut process [ {0} ] failed!!!".format(process)

    def kill_target(self):
        """
        Kill nvmf target when finish one test case
        """
        if self.backend in iscsibackend:
            self.tester.send_expect("iscsiadm -m node --logout", "# ")
            self.tester.send_expect("iscsiadm -m node -o delete", "# ")
            self.kill_dut_process("iscsi_tgt")
            time.sleep(3)
        if self.backend in nvmfbackend:
            self.tester.send_expect(
                'nvme disconnect -n "nqn.2016-06.io.spdk:cnode1"', "# ")
            self.tester.send_expect(
                'nvme disconnect -n "nqn.2016-06.io.spdk:cnode2"', "# ")
            if self.backend == "nvmf_malloc":
                number = 6
                for i in range(number):
                    idx = i + 3
                    self.tester.send_expect(
                        'nvme disconnect -n "nqn.2016-06.io.spdk:cnode{}"'.format(idx), "# ", 5)
            if self.backend == "nvmf_multiconnection":
                number = 126
                for i in range(number):
                    idx = i + 3
                    self.tester.send_expect(
                        'nvme disconnect -n "nqn.2016-06.io.spdk:cnode{}"'.format(idx), "# ", 5)
            self.kill_dut_process("nvmf_tgt")

    def fio_test(self, **fio_conf):
        """
        Run fio to do workload from nvmf for combination of io size 512 ~ 256k, queue depth 1 ~ 128
        """
        if 'test_type' not in fio_conf or \
           'io_size' not in fio_conf or \
           'queue_depth' not in fio_conf:
            self.verify(False, "fio test do not have correct keys in dict!!!")
        if not fio_conf['test_type'] or \
           not fio_conf['io_size'] or \
           not fio_conf['queue_depth']:
            self.verify(False, "fio test have null values in dict!!!")
        if 'runsize' not in fio_conf or not fio_conf['runsize']:
            fio_conf['runsize'] = '512M'
        if 'runtime' not in fio_conf or not fio_conf['runtime']:
            fio_conf['runtime'] = '10'
        if 'verify' not in fio_conf:
            fio_conf['verify'] = True
        if 'rwmixread' not in fio_conf:
            fio_conf['rwmixread'] = '50'
        test_type = str(fio_conf['test_type'])
        io_size = str(fio_conf['io_size'])
        queue_depth = str(fio_conf['queue_depth'])
        runsize = str(fio_conf['runsize'])
        runtime = str(fio_conf['runtime'])
        verify = bool(fio_conf['verify'])
        rwmixread = str(fio_conf['rwmixread'])
        if self.backend in iscsibackend:
            if self.backend == "iscsi_rxtxqueue" or self.backend == "iscsi_multiconnection":
                self.write_fio_test(
                    test_type,
                    io_size,
                    queue_depth,
                    runtime,
                    "512M",
                    verify,
                    rwmixread)
            else:
                self.write_fio_test(
                    test_type,
                    io_size,
                    queue_depth,
                    runtime,
                    "512M",
                    verify,
                    "50")
        if self.backend in nvmfbackend:
            if self.backend == "nvme_direct" or self.backend == "nvmf_multiconnection":
                self.write_fio_test(
                    test_type,
                    io_size,
                    queue_depth,
                    "20",
                    runsize,
                    verify,
                    rwmixread)
            else:
                self.write_fio_test(
                    test_type,
                    io_size,
                    queue_depth,
                    "20",
                    runsize,
                    verify,
                    "50")
        time.sleep(5)
        if self.backend in iscsibackend:
            out = Fio_iscsi_test.start_fio_client()
        if self.backend in nvmfbackend:
            out = Fio_test.start_fio_client()
        self.verify(
            "All tests passed" in out, "test_%s_%s_%s failed" %
            (test_type, io_size, queue_depth))

    def fio_read(self, io_size, queue_depth, verify=True,
                 runsize=None, runtime=None):
        if runtime is None:
            runtime = self.DEFAULT_RUN_TIME
        if runsize is None:
            runsize = self.DEFAULT_RUN_SIZE
        fio_conf = {}
        fio_conf['test_type'] = 'read'
        fio_conf['io_size'] = str(io_size)
        fio_conf['queue_depth'] = str(queue_depth)
        fio_conf['verify'] = verify
        fio_conf['runtime'] = str(runtime)
        fio_conf['runsize'] = str(runsize)
        self.fio_test(**fio_conf)

    def fio_write(self, io_size, queue_depth, verify=True,
                  runsize=None, runtime=None):
        if runtime is None:
            runtime = self.DEFAULT_RUN_TIME
        if runsize is None:
            runsize = self.DEFAULT_RUN_SIZE
        fio_conf = {}
        fio_conf['test_type'] = 'write'
        fio_conf['io_size'] = str(io_size)
        fio_conf['queue_depth'] = str(queue_depth)
        fio_conf['runtime'] = str(runtime)
        fio_conf['runsize'] = str(runsize)
        fio_conf['verify'] = verify
        self.fio_test(**fio_conf)

    def fio_rw(self, io_size, queue_depth, rwmixread,
               verify=True, runsize=None, runtime=None):
        if runtime is None:
            runtime = self.DEFAULT_RUN_TIME
        if runsize is None:
            runsize = self.DEFAULT_RUN_SIZE
        fio_conf = {}
        fio_conf['test_type'] = 'rw'
        fio_conf['io_size'] = str(io_size)
        fio_conf['queue_depth'] = str(queue_depth)
        fio_conf['runtime'] = str(runtime)
        fio_conf['runsize'] = str(runsize)
        fio_conf['verify'] = verify
        fio_conf['rwmixread'] = str(rwmixread)
        self.fio_test(**fio_conf)

    def fio_randread(self, io_size, queue_depth, verify=True,
                     runsize=None, runtime=None):
        if runtime is None:
            runtime = self.DEFAULT_RUN_TIME
        if runsize is None:
            runsize = self.DEFAULT_RUN_SIZE
        fio_conf = {}
        fio_conf['test_type'] = 'randread'
        fio_conf['io_size'] = str(io_size)
        fio_conf['queue_depth'] = str(queue_depth)
        fio_conf['runtime'] = str(runtime)
        fio_conf['runsize'] = str(runsize)
        fio_conf['verify'] = verify
        self.fio_test(**fio_conf)

    def fio_randwrite(self, io_size, queue_depth,
                      verify=True, runsize=None, runtime=None):
        if runtime is None:
            runtime = self.DEFAULT_RUN_TIME
        if runsize is None:
            runsize = self.DEFAULT_RUN_SIZE
        fio_conf = {}
        fio_conf['test_type'] = 'randwrite'
        fio_conf['io_size'] = str(io_size)
        fio_conf['queue_depth'] = str(queue_depth)
        fio_conf['runtime'] = str(runtime)
        fio_conf['runsize'] = str(runsize)
        fio_conf['verify'] = verify
        self.fio_test(**fio_conf)

    def fio_randrw(self, io_size, queue_depth, rwmixread,
                   verify=True, runsize=None, runtime=None):
        if runtime is None:
            runtime = self.DEFAULT_RUN_TIME
        if runsize is None:
            runsize = self.DEFAULT_RUN_SIZE
        fio_conf = {}
        fio_conf['test_type'] = 'randrw'
        fio_conf['io_size'] = str(io_size)
        fio_conf['queue_depth'] = str(queue_depth)
        fio_conf['runtime'] = str(runtime)
        fio_conf['runsize'] = str(runsize)
        fio_conf['verify'] = verify
        fio_conf['rwmixread'] = str(rwmixread)
        self.fio_test(**fio_conf)

    def fio_trim(self, io_size, queue_depth, verify=True,
                 runsize=None, runtime=None):
        if runtime is None:
            runtime = self.DEFAULT_RUN_TIME
        if runsize is None:
            runsize = self.TRIM_RUN_SIZE
        fio_conf = {}
        fio_conf['test_type'] = 'trim'
        fio_conf['io_size'] = str(io_size)
        fio_conf['queue_depth'] = str(queue_depth)
        fio_conf['runsize'] = str(runsize)
        fio_conf['runtime'] = str(runtime)
        fio_conf['verify'] = verify
        self.fio_test(**fio_conf)

    def fio_randtrim(self, io_size, queue_depth, verify=True,
                     runsize=None, runtime=None):
        if runtime is None:
            runtime = self.DEFAULT_RUN_TIME
        if runsize is None:
            runsize = self.TRIM_RUN_SIZE
        fio_conf = {}
        fio_conf['test_type'] = 'randtrim'
        fio_conf['io_size'] = str(io_size)
        fio_conf['queue_depth'] = str(queue_depth)
        fio_conf['runsize'] = str(runsize)
        fio_conf['runtime'] = str(runtime)
        fio_conf['verify'] = verify
        self.fio_test(**fio_conf)

    def fio_trimwrite(self, io_size, queue_depth,
                      verify=True, runsize=None, runtime=None):
        if runtime is None:
            runtime = self.DEFAULT_RUN_TIME
        if runsize is None:
            runsize = self.TRIM_RUN_SIZE
        fio_conf = {}
        fio_conf['test_type'] = 'trimwrite'
        fio_conf['io_size'] = str(io_size)
        fio_conf['queue_depth'] = str(queue_depth)
        fio_conf['runsize'] = str(runsize)
        fio_conf['runtime'] = str(runtime)
        fio_conf['verify'] = verify
        self.fio_test(**fio_conf)

    def test_fio_read_512_1_verify(self):
        self.fio_read(512, 1, True)

    def test_fio_read_4096_16_verify(self):
        self.fio_read(4096, 16, True)

    def test_fio_read_256k_64_verify(self):
        self.fio_read('256k', 64, True)

    def test_fio_write_512_1_verify(self):
        self.fio_write(512, 1, True)

    def test_fio_write_4096_16_verify(self):
        self.fio_write(4096, 16, True)

    def test_fio_write_256k_64_verify(self):
        self.fio_write('256k', 64, True)

    def test_fio_rw_512_1_verify(self):
        self.fio_rw(512, 1, True)

    def test_fio_rw_4096_16_verify(self):
        self.fio_rw(4096, 16, True)

    def test_fio_rw_256k_64_verify(self):
        self.fio_rw('256k', 64, True)

    def test_fio_randread_512_1_verify(self):
        self.fio_randread(512, 1, True)

    def test_fio_randread_4096_16_verify(self):
        self.fio_randread(4096, 16, True)

    def test_fio_randread_256k_64_verify(self):
        self.fio_randread('256k', 64, True)

    def test_fio_randwrite_512_1_verify(self):
        self.fio_randwrite(512, 1, True)

    def test_fio_randwrite_4096_16_verify(self):
        self.fio_randwrite(4096, 16, True)

    def test_fio_randwrite_256k_64_verify(self):
        self.fio_randwrite('256k', 64, True)

    def test_fio_randrw_512_1_verify(self):
        self.fio_randrw(512, 1, True)

    def test_fio_randrw_4096_16_verify(self):
        self.fio_randrw(4096, 16, True)

    def test_fio_randrw_256k_64_verify(self):
        self.fio_randrw('256k', 64, True)

    def tear_down(self):
        """
        Run after each test case.
        """
        self.kill_target()
