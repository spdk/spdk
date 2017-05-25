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
import sys
import re

FOLDERS = {
    'Framework': 'framework',
    'Testscripts': 'tests',
    'Configuration': 'conf',
    'Depends': 'dep',
    'Output': 'output',
    'NicDriver': 'nics',
}

"""
Nics and its identifiers supported by the framework.
"""
NICS = {
    'niantic': '8086:10fb',
    'ironpond': '8086:151c',
    'twinpond': '8086:1528',
    'twinville': '8086:1512',
    'sageville': '8086:1563',
    'sagepond': '8086:15ad',
    'magnolia_park': '8086:15ce',
    'springfountain': '8086:154a',
    'fortville_eagle': '8086:1572',
    'fortville_spirit': '8086:1583',
    'fortville_spirit_single': '8086:1584',
    'fortpark': '8086:374c',
    'fortpark_TLV': '8086:37d0',
    'ConnectX3': '15b3:1007',
    'ConnectX4': '15b3:1015',
    'fortville_25g': '8086:158b',
    'chelsio_40gb': '1425:5410',
    'chelsio_unknow': '1425:5010',
}

DRIVERS = {
    'niantic': 'ixgbe',
    'ironpond': 'ixgbe',
    'twinpond': 'ixgbe',
    'twinville': 'ixgbe',
    'sageville': 'ixgbe',
    'sagepond': 'ixgbe',
    'magnolia_park': 'ixgbe',
    'springfountain': 'ixgbe',
    'fortville_eagle': 'i40e',
    'fortville_spirit': 'i40e',
    'fortville_spirit_single': 'i40e',
    'fortpark': 'i40e',
    'fortpark_TLV': 'i40e',
    'ConnectX3': 'mlx4_en',
    'ConnectX4': 'mlx5_core',
    'fortville_25g': 'i40e',
    'chelsio_40gb': 'cxgb4',
    'chelsio_unknow': 'cxgb4',
}

USERNAME = 'root'

"""
Default session timeout.
"""
TIMEOUT = 15

"""
The log name seperater.
"""
LOG_NAME_SEP = '.'

"""
global environment variable
"""
SPDK_ENV_PAT = r"SPDK_*"
PERF_SETTING = "SPDK_PERF_ONLY"
FUNC_SETTING = "SPDK_FUNC_ONLY"
HOST_DRIVER_SETTING = "SPDK_HOST_DRIVER"
HOST_NIC_SETTING = "SPDK_HOST_NIC"
DEBUG_SETTING = "SPDK_DEBUG_ENABLE"
DEBUG_CASE_SETTING = "SPDK_DEBUGCASE_ENABLE"
SPDK_ERROR_ENV = "SPDK_RUNNING_ERROR"

"""
global error table
"""
SPDK_ERR_TBL = {
    "GENERIC_ERR": 1,
    "SPDK_BUILD_ERR": 2,
    "DUT_SETUP_ERR": 3,
    "TESTER_SETUP_ERR": 4,
    "SUITE_SETUP_ERR": 5,
    "SUITE_EXECUTE_ERR": 6,
}


def get_nic_name(type):
    """
    strip nic code name by nic type
    """
    for name, nic_type in NICS.items():
        if nic_type == type:
            return name
    return 'Unknown'


def get_nic_driver(pci_id):
    """
    Return linux driver for specified NICs
    """
    driverlist = dict(zip(NICS.values(), DRIVERS.keys()))
    try:
        driver = DRIVERS[driverlist[pci_id]]
    except Exception as e:
        driver = None
    return driver


def save_global_setting(key, value):
    """
    Save global setting
    """
    if re.match(SPDK_ENV_PAT, key):
        env_key = key
    else:
        env_key = "SPDK_" + key
    os.environ[env_key] = value


def load_global_setting(key):
    """
    Load global setting
    """
    if re.match(SPDK_ENV_PAT, key):
        env_key = key
    else:
        env_key = "SPDK_" + key
    if env_key in os.environ.keys():
        return os.environ[env_key]
    else:
        return ''


def report_error(error):
    """
    Report error when error occurred
    """
    if error in SPDK_ERR_TBL.keys():
        os.environ[SPDK_ERROR_ENV] = error
    else:
        os.environ[SPDK_ERROR_ENV] = "GENERIC_ERR"


def exit_error():
    """
    Set system exit value when error occurred
    """
    if SPDK_ERROR_ENV in os.environ.keys():
        ret_val = SPDK_ERR_TBL[os.environ[SPDK_ERROR_ENV]]
        sys.exit(ret_val)
    else:
        sys.exit(0)
