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

from functools import wraps
import settings
from crb import Crb
from settings import TIMEOUT

NICS_LIST = []


class NetDevice(object):

    def __init__(self, crb, domain_id, bus_id, devfun_id):
        if not isinstance(crb, Crb):
            raise Exception("  Please input the instance of Crb!!!")
        self.crb = crb
        self.domain_id = domain_id
        self.bus_id = bus_id
        self.devfun_id = devfun_id
        self.pci = domain_id + ':' + bus_id + ':' + devfun_id
        self.pci_id = get_pci_id(crb, domain_id, bus_id, devfun_id)
        self.default_driver = settings.get_nic_driver(self.pci_id)
        self.intf_name = 'N/A'
        self.intf2_name = None
        self.get_interface_name()

    def __send_expect(self, cmds, expected, timeout=TIMEOUT, alt_session=True):
        """
        Wrap the crb's session as private session for sending expect.
        """
        return self.crb.send_expect(
            cmds, expected, timeout=timeout, alt_session=alt_session)

    def nic_has_driver(func):
        """
        Check if the NIC has a driver.
        """
        @wraps(func)
        def wrapper(*args, **kwargs):
            nic_instance = args[0]
            nic_instance.current_driver = nic_instance.get_nic_driver()
            if not nic_instance.current_driver:
                return ''
            return func(*args, **kwargs)
        return wrapper

    def get_nic_driver(self):
        """
        Get the NIC driver.
        """
        return self.crb.get_pci_dev_driver(
            self.domain_id, self.bus_id, self.devfun_id)

    @nic_has_driver
    def get_interface_name(self):
        """
        Get interface name of NICs.
        """
        driver = self.current_driver
        driver_alias = driver.replace('-', '_')
        try:
            get_interface_name = getattr(
                self, 'get_interface_name_%s' %
                driver_alias)
        except Exception as e:
            generic_driver = 'generic'
            get_interface_name = getattr(
                self, 'get_interface_name_%s' %
                generic_driver)
        out = get_interface_name(self.domain_id, self.bus_id, self.devfun_id)
        if "No such file or directory" in out:
            self.intf_name = 'N/A'
        else:
            self.intf_name = out
        return self.intf_name

    def get_interface_name_generic(self, domain_id, bus_id, devfun_id):
        """
        Get the interface name by the default way.
        """
        command = 'ls --color=never /sys/bus/pci/devices/%s\:%s\:%s/net' % (
            domain_id, bus_id, devfun_id)
        return self.__send_expect(command, '# ')

    def get_interface2_name(self):
        """
        Get interface name of second port of this pci device.
        """
        return self.intf2_name


def get_pci_id(crb, domain_id, bus_id, devfun_id):
    pass


def add_to_list(host, obj):
    """
    Add NICs object to global structure
    Parameter 'host' is ip address, 'obj' is netdevice object
    """
    nic = {}
    nic['host'] = host
    nic['pci'] = obj.pci
    nic['port'] = obj
    NICS_LIST.append(nic)


def get_from_list(host, domain_id, bus_id, devfun_id):
    """
    Get NICs object from global structure
    Parameter will by host ip, pci domain id, pci bus id, pci function id
    """
    for nic in NICS_LIST:
        if host == nic['host']:
            pci = ':'.join((domain_id, bus_id, devfun_id))
            if pci == nic['pci']:
                return nic['port']
    return None


def GetNicObj(crb, domain_id, bus_id, devfun_id):
    """
    Get NICs object. If NICs has been initialized, just return object.
    """
    obj = get_from_list(crb.crb['My IP'], domain_id, bus_id, devfun_id)
    if obj:
        return obj
    pci_id = get_pci_id(crb, domain_id, bus_id, devfun_id)
    nic = settings.get_nic_name(pci_id)
    obj = NetDevice(crb, domain_id, bus_id, devfun_id)
    add_to_list(crb.crb['My IP'], obj)
    return obj
