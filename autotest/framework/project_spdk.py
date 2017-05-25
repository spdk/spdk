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

import os
from settings import NICS, DRIVERS
from settings import load_global_setting, HOST_DRIVER_SETTING
from dut import Dut
from tester import Tester


class SPDKdut(Dut):
    """
    SPDK project class will be called set_target function to setup
    build, memory and kernel module.
    """

    def __init__(self, crb, serializer):
        super(SPDKdut, self).__init__(crb, serializer)
        self.testpmd = None

    def set_target(self, target):
        self.target = target
        drivername = load_global_setting(HOST_DRIVER_SETTING)
        if drivername == DRIVERS['ConnectX4']:
            out = self.send_expect("lsmod | grep mlx5_ib", "#")
            if "mlx5_ib" not in out:
                self.send_expect("modprobe mlx5_core", "#", 70)
                self.send_expect("modprobe mlx5_ib", "#", 70)
        if drivername == DRIVERS['ConnectX3']:
            out = self.send_expect("lsmod | grep mlx4_ib", "#")
            if "mlx4_ib" not in out:
                self.send_expect("modprobe mlx4_en", "#", 70)
                self.send_expect("modprobe mlx4_core", "#", 70)
                self.send_expect("modprobe mlx4_ib", "#", 70)
        if drivername == DRIVERS['chelsio_40gb']:
            out = self.send_expect("lsmod | grep iw_cxgb4", "#")
            if "iw_cxgb4" not in out:
                self.send_expect("modprobe cxgb4", "#", 70)
                self.send_expect("modprobe iw_cxgb4", "#", 70)
        if not self.skip_setup:
            self.build_install_spdk(target)
        self.setup_modules(target)

    def setup_modules(self, target):
        drivername = load_global_setting(HOST_DRIVER_SETTING)
        if drivername == "ConnectX4" or "ConnectX3":
            out = self.send_expect("lsmod | grep ib_cm", "#")
            if "ib_cm" not in out:
                self.send_expect("modprobe ib_addr", "#", 70)
                self.send_expect("modprobe ib_cm", "#", 70)
                self.send_expect("modprobe ib_core", "#", 70)
                self.send_expect("modprobe ib_mad", "#", 70)
                self.send_expect("modprobe ib_sa", "#", 70)
                self.send_expect("modprobe ib_ucm", "#", 70)
                self.send_expect("modprobe ib_umad", "#", 70)
                self.send_expect("modprobe ib_uverbs", "#", 70)
                self.send_expect("modprobe iw_cm", "#", 70)
                self.send_expect("modprobe rdma_cm", "#", 70)
                self.send_expect("modprobe rdma_ucm", "#", 70)
                print "         load some kernel modules"
            print "         kernel modules has loaded, eg: ib_cm"

    def build_install_spdk(self, target, extra_options=''):
        self.send_expect("NRHUGE=12288 %s" % r'./scripts/setup.sh', "#", 200)
        self.send_expect("make clean", "#", 20)
        self.send_expect("./configure", "#", 100)
        out = self.send_expect("make -j", "# ", 100)
        if("Error" in out or "No rule to make" in out):
            self.logger.error("ERROR - try to compile again")
            out = self.send_expect("make", "# ", 100)
        assert ("Error" not in out), "Compilation error..."
        assert ("No rule to make" not in out), "No rule to make error..."

    def prepare_package(self):
        if not self.skip_setup:
            depot = "../dep"
            gitLabel = "master"
            gitLabel1 = "spdk-17.02"
            gitURL = r"https://github.com/spdk/spdk.git"
            gitURL1 = r"https://github.com/spdk/dpdk.git"
            gitPrefix = r"spdk/"
            gitPrefix1 = r"dpdk/"
            package = r"../dep/spdk.tar.gz"
            package1 = r"../dep/dpdk.tar.gz"
            if os.path.exists("%s/%s" % (depot, gitPrefix)) is True:
                ret = os.system(
                    "cd %s/%s && git pull --force" %
                    (depot, gitPrefix))
            else:
                print "git clone %s %s/%s" % (gitURL, depot, gitPrefix)
                ret = os.system(
                    "git clone %s %s/%s" %
                    (gitURL, depot, gitPrefix))
            if ret is not 0:
                print "Clone spdk failed!!!"
                raise EnvironmentError
            if os.path.exists("%s/%s" % (depot, gitPrefix1)) is True:
                ret1 = os.system(
                    "cd %s/%s && git pull --force" %
                    (depot, gitPrefix1))
            else:
                print "git clone %s %s/%s" % (gitURL1, depot, gitPrefix1)
                ret1 = os.system(
                    "git clone %s %s/%s" %
                    (gitURL1, depot, gitPrefix1))
            if ret1 is not 0:
                print "Clone spdk failed!!!"
                raise EnvironmentError
            ret = os.system(
                "cd %s/%s && git archive --format=tar.gz --prefix=%s/ %s -o ../%s" %
                (depot, gitPrefix, gitPrefix, gitLabel, package))
            if ret is not 0:
                print "Zip spdk failed!!!"
                raise EnvironmentError
            assert (os.path.isfile(package) is True), "Invalid spdk package"
            ret1 = os.system(
                "cd %s/%s && git archive --format=tar.gz --prefix=%s/ %s -o ../%s" %
                (depot, gitPrefix1, gitPrefix1, gitLabel1, package1))
            if ret1 is not 0:
                print "Zip dpdk failed!!!"
                raise EnvironmentError
            assert (os.path.isfile(package1) is True), "Invalid dpdk package"

            p_dir, _ = os.path.split(self.base_dir)
            q_dir, _ = os.path.split(self.dpdk_dir)
            dst_dir = "/tmp/"
            out = self.send_expect(
                "ls %s && cd %s" %
                (dst_dir, p_dir), "#", verify=True)
            if out == -1:
                raise ValueError("Directiry %s or %s does not exist,"
                                 "please check params -d"
                                 % (p_dir, dst_dir))
            self.session.copy_file_to(package, dst_dir)
            self.session.copy_file_to(package1, dst_dir)
            self.send_expect("ulimit -c unlimited", "#")
            self.send_expect("rm -rf %s" % self.base_dir, "#")
            out = self.send_expect("tar zxf %s%s -C %s" %
                                   (dst_dir, package.split('/')[-1], p_dir), "# ", 20, verify=True)
            if out == -1:
                raise ValueError("Extract spdk package to %s failure,"
                                 "please check params -d"
                                 % (p_dir))
            self.send_expect("rm -rf %s" % self.dpdk_dir, "#")
            out1 = self.send_expect("tar zxf %s%s -C %s" %
                                    (dst_dir, package1.split('/')[-1], q_dir), "# ", 20, verify=True)
            if out1 == -1:
                raise ValueError("Extract spdk package to %s failure,"
                                 "please check params -d"
                                 % (q_dir))
            out = self.send_expect("cd %s" % self.base_dir,
                                   "# ", 20, verify=True)
            if out == -1:
                raise ValueError("spdk dir %s mismatch, please check params -d"
                                 % self.base_dir)

    def prerequisites(self):
        self.prepare_package()
        self.dut_prerequisites()


class SPDKtester(Tester):

    def __init__(self, crb, serializer):
        self.NAME = "tester"
        super(SPDKtester, self).__init__(crb, serializer)

    def prerequisites(self, perf_test=False):
        self.tester_prerequisites()
