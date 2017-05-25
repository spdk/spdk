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
SPDK Test suite.
This TestSuite runs the fiotest tests using linux fio.
"""

from test_case import TestCase
from Test_base_fio import TestFio


class TestNvmeFio_nvmf(TestFio, TestCase):

    def set_up_all(self):
        """
        Run at the start of each test suite.
        fio Prerequisites
        """
        self.backend = 'nvme_virtual'
        # change 8192M for daily testing
        # change 40960M for weekend testing
        self.DEFAULT_RUN_SIZE = '512M'
        self.TRIM_RUN_SIZE = '512M'
        self.DEFAULT_RUN_TIME = None
        super(
            TestNvmeFio_nvmf,
            self).set_up_all(
            self,
            self.backend,
            self.DEFAULT_RUN_SIZE,
            self.DEFAULT_RUN_TIME)

    def set_up(self):
        """
        Run before each test case.
        """
        super(TestNvmeFio_nvmf, self).set_up()

    def test_z_fio_randtrim_512_1(self):
        self.fio_randtrim(512, 1, False, self.TRIM_RUN_SIZE)

    def test_z_fio_randtrim_4096_16(self):
        self.fio_randtrim(4096, 16, False, self.TRIM_RUN_SIZE)

    def test_z_fio_randtrim_256k_64(self):
        self.fio_randtrim('256k', 64, False, self.TRIM_RUN_SIZE)

    def test_z_fio_trim_512_1(self):
        self.fio_randtrim(512, 1, False, self.TRIM_RUN_SIZE)

    def test_z_fio_trim_4096_16(self):
        self.fio_trim(4096, 16, False, self.TRIM_RUN_SIZE)

    def test_z_fio_trim_256k_64(self):
        self.fio_trim('256k', 64, False, self.TRIM_RUN_SIZE)

    def test_z_fio_trimwrite_512_1(self):
        self.fio_trimwrite(512, 1, False, self.TRIM_RUN_SIZE)

    def test_z_fio_trimwrite_4096_16(self):
        self.fio_trimwrite(4096, 16, False, self.TRIM_RUN_SIZE)

    def test_z_fio_trimwrite_256k_64(self):
        self.fio_trimwrite('256k', 64, False, self.TRIM_RUN_SIZE)

    def tear_down(self):
        """
        Run after each test case.
        """
        super(TestNvmeFio_nvmf, self).tear_down()
