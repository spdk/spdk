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

import re
import sys
import os
import ConfigParser
from StringIO import StringIO
from subprocess import check_call, call, check_output, Popen, PIPE


class ParseConfig(object):

    def __init__(self, config_path):
        self.conf = ConfigParser.ConfigParser()
        self.conf_path = config_path
        self.conf.read(self.conf_path)

    def get_sections(self):
        return self.conf.sections()

    def get_section_options(self, section):
        return self.conf.options(section)

    def get_section_items(self, section):
        return self.conf.items(section)

    def get_section_option(self, section, option):
        return self.conf.get(section, option)

    def add_section(self, section):
        if self.conf.has_section(section):
            pass
        else:
            self.conf.add_section(section)

    def set_section_option(self, section, option, value):
        self.add_section(section)
        self.conf.set(section, option, value)

    def write_file(self):
        self.conf.write(open(self.conf_path, "w"))


def write_fio_config(config_path, section, **kwargs):
    """
    kwargs:
        {option1: value,
         option2: value,
        }
    """
    config = ParseConfig(config_path)
    for option in kwargs.keys():
        config.set_section_option(section, option, kwargs[option])

    config.write_file()


def generate_iscsi_file(backend, filename):
    check_call("sed -i '/\[Nvme/,$d' " + filename, shell=True)
    if backend == "iscsi_nvme" or backend == "iscsi_rxtxqueue":
        output = check_output("lspci -nnn", shell=True)
        bus_numbers = re.findall(
            "([0-9][0-9]:[0-9][0-9].[0-9]) Non-Volatile memory controller", output)
        nvme_ctrl_num = len(bus_numbers)
        all_luns = int(nvme_ctrl_num)

        filedesc = open(filename, 'a')
        filedesc.write("\n[Nvme] \n")
        for i, value in enumerate(bus_numbers):
            filedesc.write(
                '\n  TransportId "trtype:PCIe traddr:0000:{}" Nvme{} \n'.format(value, i))
        filedesc.write("  RetryCount {} \n".format(nvme_ctrl_num))
        filedesc.write("  Timeout 0 \n")
        filedesc.write("  ActionOnTimeout None \n")
        filedesc.write("  AdminPollRate 100000 \n")
        filedesc.write("  HotplugEnable Yes \n")
        idx = 0
        for i in range(all_luns):
            target_id = idx + 1
            filedesc.write("\n[TargetNode" + str(target_id) + "]\n")
            filedesc.write("  TargetName disk" + str(target_id) + "\n")
            filedesc.write("  Mapping PortalGroup1 InitiatorGroup1\n")
            filedesc.write("  AuthMethod Auto\n")
            filedesc.write("  AuthGroup  AuthGroup1\n")
            filedesc.write("  UseDigest Auto\n")
            filedesc.write("  QueueDepth 128\n")
            filedesc.write("  LUN0 Nvme" + str(idx) + "n1" + "\n")
            idx = idx + 1
        filedesc.close()

    if backend == "iscsi_aiobackend":
        filedesc = open(filename, 'a')
        filedesc.write("\n[AIO] \n")
        filedesc.write("  AIO /dev/sdf AIO0\n")
        filedesc.write("  AIO /dev/sde AIO1\n")

        all_luns = 2
        idx = 0
        for i in range(all_luns):
            target_id = idx + 1
            filedesc.write("\n[TargetNode" + str(target_id) + "]\n")
            filedesc.write("  TargetName disk" + str(target_id) + "\n")
            filedesc.write("  Mapping PortalGroup1 InitiatorGroup1\n")
            filedesc.write("  AuthMethod Auto\n")
            filedesc.write("  AuthGroup  AuthGroup1\n")
            filedesc.write("  UseDigest Auto\n")
            filedesc.write("  QueueDepth 128\n")
            filedesc.write("  LUN0 AIO" + str(idx) + "\n")
            idx = idx + 1
        filedesc.close()

    if backend == "iscsi_malloc":
        all_luns = 2
        idx = 0
        filedesc = open(filename, 'a')
        filedesc.write("\n[Malloc] \n")
        filedesc.write("  NumberOfLuns 2\n")
        filedesc.write("  LunSizeInMB 128\n")
        filedesc.write("  BlockSize 512\n")
        for i in range(all_luns):
            target_id = idx + 1
            filedesc.write("\n[TargetNode" + str(target_id) + "]\n")
            filedesc.write("  TargetName disk" + str(target_id) + "\n")
            filedesc.write("  Mapping PortalGroup1 InitiatorGroup1\n")
            filedesc.write("  AuthMethod Auto\n")
            filedesc.write("  AuthGroup  AuthGroup1\n")
            filedesc.write("  UseDigest Auto\n")
            filedesc.write("  QueueDepth 128\n")
            filedesc.write("  LUN0 Malloc" + str(idx) + "\n")
            idx = idx + 1
        filedesc.close()

    if backend == "iscsi_multiconnection":
        output = check_output("lspci -nnn", shell=True)
        bus_numbers = re.findall(
            "([0-9][0-9]:[0-9][0-9].[0-9]) Non-Volatile memory controller", output)
        nvme_ctrl_num = len(bus_numbers)
        all_luns = int(nvme_ctrl_num)

        filedesc = open(filename, 'a')
        filedesc.write("\n[Nvme] \n")
        for i, value in enumerate(bus_numbers):
            filedesc.write(
                '  TransportID "trtype:PCIe traddr:0000:{}" Nvme{}\n'.format(value, i))
        filedesc.write("  RetryCount 128 \n")
        filedesc.write("  Timeout 0 \n")
        filedesc.write("  ActionOnTimeout None \n")
        filedesc.write("  AdminPollRate 100000 \n")
        filedesc.write("  HotplugEnable Yes \n")
        filedesc.write("\n[Split] \n")
        filedesc.write("  Split Nvme0n1 22 1 \n")
        filedesc.write("  Split Nvme1n1 22 1 \n")
        filedesc.write("  Split Nvme2n1 22 1 \n")
        filedesc.write("  Split Nvme3n1 22 1 \n")
        filedesc.write("  Split Nvme4n1 22 1 \n")
        filedesc.write("  Split Nvme5n1 18 1 \n")
        idx = 0
        target_id = 1
        all_nodes = 22
        for i in range(all_luns):
            node = 0
            for i in range(all_nodes):
                filedesc.write("\n[TargetNode" + str(target_id) + "]\n")
                filedesc.write("  TargetName disk" + str(target_id) + "\n")
                filedesc.write("  Mapping PortalGroup1 InitiatorGroup1\n")
                filedesc.write("  AuthMethod Auto\n")
                filedesc.write("  AuthGroup  AuthGroup1\n")
                filedesc.write("  UseDigest Auto\n")
                filedesc.write("  QueueDepth 32\n")
                filedesc.write("  LUN0 Nvme" + str(idx) +
                               "n1p" + str(node) + "\n")
                node = node + 1
                target_id = target_id + 1
            idx = idx + 1
        filedesc.close()
        check_call("sed -i '/\[TargetNode129/,$d' " + filename, shell=True)


def generate_nvmf_tgt_file(backend, filename):
    if backend == "nvme_direct":
        check_call("sed -i '/\[Subsystem/,$d' " + filename, shell=True)
        output = check_output("lspci -nnn", shell=True)
        bus_numbers = re.findall(
            "([0-9][0-9]:[0-9][0-9].[0-9]) Non-Volatile memory controller", output)
        nvme_ctrl_num = len(bus_numbers)
        all_luns = int(nvme_ctrl_num)
        idx = 0
        filedesc = open(filename, 'a')
        for i, value in enumerate(bus_numbers):
            target_id = idx + 1
            filedesc.write("\n[Subsystem" + str(target_id) + "]\n")
            filedesc.write("  NQN nqn.2016-06.io.spdk:cnode" +
                           str(target_id) + "\n")
            filedesc.write("  Core 0\n")
            filedesc.write("  Mode Direct\n")
            filedesc.write("  Listen RDMA 192.168.3.11:4420\n")
            if i == idx:
                filedesc.write("  Nvme 0000:{} \n".format(value, i))
            idx = idx + 1
        filedesc.close()

    if backend == "nvme_virtual":
        check_call("sed -i '/\[Nvme/,$d' " + filename, shell=True)
        output = check_output("lspci -nnn", shell=True)
        bus_numbers = re.findall(
            "([0-9][0-9]:[0-9][0-9].[0-9]) Non-Volatile memory controller", output)
        nvme_ctrl_num = len(bus_numbers)
        all_luns = int(nvme_ctrl_num)
        idx = 0
        filedesc = open(filename, 'a')
        filedesc.write("\n[Nvme] \n")
        for i, value in enumerate(bus_numbers):
            filedesc.write(
                '\n  TransportId "trtype:PCIe traddr:0000:{}" Nvme{} \n'.format(value, i))
        filedesc.write("\n  NvmeRetryCount 2 \n")
        filedesc.write("\n  ResetControllerOnTimeout Yes \n")
        filedesc.write("\n  NvmeTimeoutValue 30 \n")
        filedesc.write("\n  AdminPollRate 100000 \n")
        for i in range(all_luns):
            target_id = idx + 1
            filedesc.write("\n[Subsystem" + str(target_id) + "]\n")
            filedesc.write("  NQN nqn.2016-06.io.spdk:cnode" +
                           str(target_id) + "\n")
            filedesc.write("  Core 0\n")
            filedesc.write("  Mode Virtual\n")
            filedesc.write("  Listen RDMA 192.168.3.11:4420\n")
            filedesc.write("  SN SPDK" + str(target_id) + "\n")
            filedesc.write("  Namespace Nvme" + str(idx) + "n1" + "\n")
            idx = idx + 1
        filedesc.close()

    if backend == "nvmf_malloc":
        check_call("sed -i '/\[Split/,$d' " + filename, shell=True)
        all_luns = 8
        idx = 0
        filedesc = open(filename, 'a')
        for i in range(all_luns):
            target_id = idx + 1
            filedesc.write("\n[Subsystem" + str(target_id) + "]\n")
            filedesc.write(
                "  NQN nqn.2016-06.io.spdk:cnode" +
                str(target_id) +
                "\n")
            filedesc.write("  Core 0\n")
            filedesc.write("  Mode Virtual\n")
            filedesc.write("  Listen RDMA 192.168.3.11:4420\n")
            filedesc.write("  SN SPDK" + str(target_id) + "\n")
            filedesc.write("  Namespace Malloc" + str(idx) + "\n")
            idx = idx + 1
        filedesc.close()

    if backend == "nvmf_multiconnection":
        check_call("sed -i '/\[Split/,$d' " + filename, shell=True)
        all_luns = 128
        idx = 0
        filedesc = open(filename, 'a')
        for i in range(all_luns):
            target_id = idx + 1
            filedesc.write("\n[Subsystem" + str(target_id) + "]\n")
            filedesc.write(
                "  NQN nqn.2016-06.io.spdk:cnode" +
                str(target_id) +
                "\n")
            filedesc.write("  Core 0\n")
            filedesc.write("  Mode Virtual\n")
            filedesc.write("  Listen RDMA 192.168.3.11:4420\n")
            filedesc.write("  SN SPDK" + str(target_id) + "\n")
            filedesc.write("  Namespace Malloc" + str(idx) + "\n")
            idx = idx + 1
        filedesc.close()

    if backend == "nvmf_aiobackend":
        check_call("sed -i '/\[Subsystem/,$d' " + filename, shell=True)
        filedesc = open(filename, 'a')
        all_luns = 2
        idx = 0
        for i in range(all_luns):
            target_id = idx + 1
            filedesc.write("\n[Subsystem" + str(target_id) + "]\n")
            filedesc.write("  NQN nqn.2016-06.io.spdk:cnode" +
                           str(target_id) + "\n")
            filedesc.write("  Core 0\n")
            filedesc.write("  Mode Virtual\n")
            filedesc.write("  Listen RDMA 192.168.3.11:4420\n")
            filedesc.write("  SN SPDK" + str(target_id) + "\n")
            filedesc.write("  Namespace AIO" + str(idx) + "\n")
            idx = idx + 1
        filedesc.close()

if __name__ == "__main__":
    if (len(sys.argv) < 4):
        print "usage:"
        print "  " + sys.argv[0] + " <method name> <backend name> "
        sys.exit(1)

    method_name = sys.argv[1]
    backend_name = sys.argv[2]

    if method_name == "generate_iscsi_file":
        iscsi_file = sys.argv[3]
        generate_iscsi_file(backend_name, iscsi_file)

    if method_name == "generate_nvmf_tgt_file":
        nvmf_file = sys.argv[3]
        generate_nvmf_tgt_file(backend_name, nvmf_file)

    if method_name == "write_fio_config":
        write_fio_config(config_file, sections, **kwargs)
