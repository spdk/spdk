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
from time import sleep
from settings import NICS, DRIVERS
from crb import Crb
from net_device import GetNicObj


class Dut(Crb):

    PCI_DEV_CACHE_KEY = 'dut_pci_dev_info'

    def __init__(self, crb, serializer):
        self.NAME = 'dut'
        super(Dut, self).__init__(crb, serializer, self.NAME)
        self.host_init_flag = False
        self.tester = None
        self.ports_info = None
        self.ports_map = []

    def get_ip_address(self):
        """
        Get DUT's ip address.
        """
        return self.crb['IP']

    def get_password(self):
        """
        Get DUT's login password.
        """
        return self.crb['pass']

    def dut_prerequisites(self):
        """
        Configure DUT's NICs.
        """
        self.pci_devices_information()
        self.restore_interfaces()

    def get_ports(self):
        """
        Get DUT's NICs information.
        """
        ethname = []
        drivername = []
        driver = []
        nic = []
        for key in DRIVERS:
            nic = key
            driver = DRIVERS[key]
        for (pci_bus, pci_id) in self.pci_devices_info:
            addr_array = pci_bus.split(':')
            port = GetNicObj(self, addr_array[0], addr_array[1], addr_array[2])
            eth = port.get_interface_name()
            self.enable_ipv6(eth)
            if len(eth) >= 12:
                status1 = eth.split()
                self.enable_ipv6(status1[0])
                self.enable_ipv6(status1[1])
                out1 = self.send_expect("ethtool -i %s" % status1[0], "# ")
                out2 = self.send_expect("ethtool -i %s" % status1[1], "# ")
                status2 = re.findall(r"driver:\s+(.*)", out1)
                status3 = re.findall(r"driver:\s+(.*)", out2)
                if status2:
                    drivername.append(status2[0])
                    ethname.append(status1[0])
                if status3:
                    drivername.append(status3[0])
                    ethname.append(status1[1])
                if not status2:
                    self.logger.error("ERROR: unexpected output")
                if not status3:
                    self.logger.error("ERROR: unexpected output")
            else:
                out = self.send_expect("ethtool -i %s" % eth, "# ")
                status = re.findall(r"driver:\s+(.*)", out)
                if status:
                    drivername.append(status[0])
                    ethname.append(eth)
                if not status:
                    self.logger.error("ERROR: unexpected output")
        return ethname, drivername

    def restore_interfaces(self):
        """
        Restore Linux interfaces.
        """
        if self.skip_setup:
            return
        try:
            for (pci_bus, pci_id) in self.pci_devices_info:
                addr_array = pci_bus.split(':')
                port = GetNicObj(self, addr_array[0], addr_array[
                                 1], addr_array[2])
                itf = port.get_interface_name()
                self.enable_ipv6(itf)
                self.send_expect("ifconfig %s up" % itf, "# ")
                if port.get_interface2_name():
                    itf = port.get_interface2_name()
                    self.enable_ipv6(itf)
                    self.send_expect("ifconfig %s up" % itf, "# ")
        except Exception as e:
            self.logger.error("   !!! Restore ITF: " + e.message)
        sleep(2)

    def close(self):
        """
        Close ssh session of DUT.
        """
        if self.session:
            self.session.close()
            self.session = None
        if self.alt_session:
            self.alt_session.close()
            self.alt_session = None
        if self.host_init_flag:
            self.host_session.close()

    def crb_exit(self):
        """
        Recover all resource before crb exit
        """
        self.close()
