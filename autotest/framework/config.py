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

"""
Generic port and crbs configuration file load function.
"""

import ConfigParser
from exception import ConfigParseException

CRBCONF = "crbs.cfg"


class UserConf():

    def __init__(self, config):
        self.conf = ConfigParser.SafeConfigParser()
        load_files = self.conf.read(config)
        if load_files == []:
            print "FAILED LOADING %s!!!" % config
            self.conf = None
            raise ConfigParseException(config)

    def get_sections(self):
        if self.conf is None:
            return None
        return self.conf.sections()

    def load_section(self, section):
        if self.conf is None:
            return None
        items = None
        for conf_sect in self.conf.sections():
            if conf_sect == section:
                items = self.conf.items(section)
        return items

class CrbsConf(UserConf):

    DEF_CRB = {'IP': '', 'user': '', 'pass': '', 'tester IP': '', 'tester pass': ''}

    def __init__(self, crbs_conf=CRBCONF):
        self.config_file = crbs_conf
        self.crbs_cfg = []
        try:
            self.crbs_conf = UserConf(self.config_file)
        except ConfigParseException:
            self.crbs_conf = None
            raise ConfigParseException

    def load_crbs_config(self):
        sections = self.crbs_conf.get_sections()
        if not sections:
            return self.crbs_cfg
        for name in sections:
            crb = self.DEF_CRB.copy()
            crb_confs = self.crbs_conf.load_section(name)
            if not crb_confs:
                continue
            for conf in crb_confs:
                key, value = conf
                if key == 'dut_ip':
                    crb['IP'] = value
                elif key == 'dut_user':
                    crb['user'] = value
                elif key == 'dut_passwd':
                    crb['pass'] = value
                elif key == 'tester_ip':
                    crb['tester IP'] = value
                elif key == 'tester_passwd':
                    crb['tester pass'] = value
            self.crbs_cfg.append(crb)
        return self.crbs_cfg
