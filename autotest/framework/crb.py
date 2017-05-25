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
from settings import TIMEOUT
from ssh_connection import SSHConnection
from logger import getLogger


class Crb(object):

    """
    Get the information of NIC and setup for SPDK.
    """

    def __init__(self, crb, serializer, name):
        self.crb = crb
        self.skip_setup = False
        self.serializer = serializer
        self.ports_info = None
        self.sessions = []
        self.name = name
        self.logger = getLogger(name)
        self.session = SSHConnection(self.get_ip_address(), name,
                                     self.get_password())
        self.session.init_log(self.logger)
        self.alt_session = SSHConnection(
            self.get_ip_address(),
            name + '_alt',
            self.get_password())
        self.alt_session.init_log(self.logger)

    def send_expect(self, cmds, expected, timeout=TIMEOUT,
                    alt_session=False, verify=False):
        """
        Send commands to target and return string before expected string.
        If not, TimeoutException will be raised.
        """
        if alt_session:
            return self.alt_session.session.send_expect(
                cmds, expected, timeout, verify)
        return self.session.send_expect(cmds, expected, timeout, verify)

    def get_session_output(self, timeout=TIMEOUT):
        """
        Get session output message before timeout
        """
        return self.session.get_session_before(timeout)

    def set_speedup_options(self, skip_setup):
        """
        Configure skip network topology scan or skip SPDK packet setup.
        """
        self.skip_setup = skip_setup

    def set_directory(self, base_dir):
        """
        Set SPDK package folder name.
        """
        self.base_dir = base_dir

    def set_path(self, dpdk_dir):
        """
        Add DPDK package path name.
        """
        self.dpdk_dir = dpdk_dir

    def pci_devices_information(self):
        self.pci_devices_information_uncached()
        self.serializer.save(self.PCI_DEV_CACHE_KEY, self.pci_devices_info)

    def pci_devices_information_uncached(self):
        out = self.send_expect(
            "lspci -Dnn | grep -i eth", "# ", alt_session=True)
        rexp = r"([\da-f]{4}:[\da-f]{2}:[\da-f]{2}.\d{1}) .*Eth.*?ernet .*?([\da-f]{4}:[\da-f]{4})"
        pattern = re.compile(rexp)
        match = pattern.findall(out)
        self.pci_devices_info = []
        for i in range(len(match)):
            self.pci_devices_info.append((match[i][0], match[i][1]))

    def get_pci_dev_driver(self, domain_id, bus_id, devfun_id):
        out = self.send_expect("cat /sys/bus/pci/devices/%s\:%s\:%s/uevent" %
                               (domain_id, bus_id, devfun_id), "# ", alt_session=True)
        rexp = r"DRIVER=(.+?)\r"
        pattern = re.compile(rexp)
        match = pattern.search(out)
        if not match:
            return None
        return match.group(1)

    def enable_ipv6(self, intf):
        """
        Enable ipv6 of of specified interface
        """
        if intf != 'N/A':
            self.send_expect("sysctl net.ipv6.conf.%s.disable_ipv6=0" %
                             intf, "# ", alt_session=True)
