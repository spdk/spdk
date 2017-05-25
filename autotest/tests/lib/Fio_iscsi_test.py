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

from subprocess import check_call, call, check_output, Popen, PIPE
import xmlrpclib
import re
import os
import sys
import time
import datetime
import ConfigParser
import socket
import threading

fio_template = """
[global]
thread=1
invalidate=1
rw=%(testtype)s
time_based=1
runtime=%(run_time)d
rwmixread=%(rw_mixread)d
ioengine=libaio
direct=1
bs=%(blocksize)d
iodepth=%(iodepth)d
direct=1
%(verify)s
verify_dump=1
verify_async=10
%(trim)s

"""

verify_template = """
do_verify=1
verify=pattern
verify_pattern="spdk"
"""

verify_write_template = """
do_verify=0
verify=pattern
verify_pattern="spdk"
"""

fio_job_template = """
[job%(jobnumber)d]
filename=%(device)s

"""

trim_template = """
trim_percentage=10
trim_verify_zero=1

"""


def start_fio_client():
    start_time = datetime.datetime.now()
    path = os.path.realpath(__file__)
    current_path = os.path.dirname(path)
    config = ConfigParser.ConfigParser()
    file_name = current_path + '/Fio_test.conf'
    config.readfp(open(file_name))
    io_range = get_range(config, 'test', 'io_size')
    queue_range = get_range(config, 'test', 'queue_depth')
    io_sizes = list(power_of_2_range(io_range[0], io_range[1]))
    queue_depths = list(power_of_2_range(queue_range[0], queue_range[1]))
    run_time = config.get('test', 'runtime')
    run_time = int(run_time)
    verify = config.get('test', 'verify')
    verify = str(verify)
    if verify == "False":
        verify = False
    else:
        verify = True
    rwmixread = config.get('test', 'rwmixread')
    rwmixread = int(rwmixread)
    log = ""
    target_ip = config.get('target', 'iscsi_addr')
    target = xmlrpclib.ServerProxy(
        config.get('target', 'iscsi_testserver_addr'))
    time.sleep(5)
    devices = get_target_devices()
    print "Found devices: {0}".format(devices)
    test_types = config.get('test', 'test_types').split()
    old_config = configure_devices(devices)
    if config.has_option('test', 'fio_path'):
        fio_executable = config.get('test', 'fio_path')
    else:
        fio_executable = '/usr/bin/fio'
    try:
        device_paths = ['/dev/' + dev for dev in devices]
        for size in io_sizes:
            for q_depth in queue_depths:
                for test in test_types:
                    print size, q_depth
                    sys.stdout.flush()
                    if verify:
                        if test == "read" or test == "randread":
                            for singledevice in device_paths:
                                singledevice = [singledevice]
                                fio = Popen([fio_executable, '-'], stdin=PIPE)
                                write_depth = 1
                                if size == 512:
                                    run_time_data = 600
                                elif size == 4096:
                                    run_time_data = 1000
                                elif size == 262144:
                                    run_time_data = 1800
                                test_data = "write"
                                fio.communicate(
                                    create_fio_config(
                                        size,
                                        int(write_depth),
                                        singledevice,
                                        test_data,
                                        int(run_time_data),
                                        verify,
                                        rwmixread,
                                        writedata=True))
                                fio.stdin.close()
                                rc = fio.wait()
                                print "FIO write operation completed with code {0}\n".format(rc)
                    sys.stdout.flush()
                    time.sleep(3)
                    fio = Popen([fio_executable, '-'], stdin=PIPE)
                    fio.communicate(create_fio_config(
                        size, q_depth, device_paths, test, run_time, verify, rwmixread, writedata=False))
                    fio.stdin.close()
                    rc = fio.wait()
                    print "FIO completed with code {0}\n".format(rc)
                    sys.stdout.flush()
                    time.sleep(1)
                    if rc != 0:
                        log += "Failed %s at Size %d, queue depth %d\n" % (
                            test, size, q_depth)
    finally:
        end_time = datetime.datetime.now()
        duration = end_time - start_time
        print "duration is {0}".format(duration)
        restore_configuration(devices, old_config)
        print "*" * 10
        if len(log) == 0:
            print "All tests passed"
            return "All tests passed"
        else:
            print log
            return log


def get_fio_version():
    output = check_output('fio --version', shell=True)
    version = re.findall("fio-([0-9]+.*)", output)
    tupleversion = version[0]
    versionstr = tupleversion
    tupleversion = tupleversion.split('.')
    for i in range(len(tupleversion)):
        tupleversion[i] = int(tupleversion[i])
    tupleversion = tuple(tupleversion)
    if tupleversion < (2, 1, 14):
        print "fio version must be 2.1.14 or above. Your fio version is {0}".format(versionstr)
        sys.exit(1)
    else:
        print "fio version is Ok"


def get_target_devices():
    output = check_output('iscsiadm -m session -P 3', shell=True)
    return re.findall("Attached scsi disk (sd[a-z]+)", output)


def create_fio_config(size, q_depth, devices, test,
                      run_time, verify, rwmixread, writedata=False):
    if not verify:
        verifyfio = ""
    else:
        if writedata:
            verifyfio = verify_write_template
        else:
            verifyfio = verify_template
    if test == "trim" or test == "randtrim":
        trim_tem = trim_template
    else:
        trim_tem = ""
    fiofile = fio_template % {"blocksize": size, "iodepth": q_depth, "testtype": test,
                              "run_time": run_time, "rw_mixread": rwmixread, "verify": verifyfio, "trim": trim_tem}
    for (i, dev) in enumerate(devices):
        fiofile += fio_job_template % {"jobnumber": i, "device": dev}
    return fiofile


def configure_devices(devices):
    config = {}
    disable_merge_file = "/sys/block/%s/queue/nomerges"
    for dev in devices:
        filename = disable_merge_file % dev
        if os.path.isfile(filename):
            time.sleep(2)
            f = open(filename, 'r+b')
            config[filename] = f.read()
            f.seek(0)
            f.write('2')  # The value 2 full disables merges.
            f.close()
    return config


def restore_configuration(devices, old_config):
    for filename in old_config:
        if os.path.isfile(filename):
            f = open(filename, 'wb')
            f.write(old_config[filename])
            f.close()


def power_of_2_range(start, end):
    n = convert_units(start)
    while n <= convert_units(end):
        yield n
        n = n * 2


def convert_units(num):
    if isinstance(num, type(str())):
        if not num.isdigit():
            multipliers = {'K': 1024, 'M': 1024**2, 'G': 1024**3, 'T': 1024**4}
            x = int(num[:-1])
            prefix = num[-1].upper()
            return x * multipliers[prefix]
        else:
            return int(num)
    else:
        return num


def get_range(config, section, item):
    range_string = config.get(section, item).split('-')
    range_list = [x.strip() for x in range_string]
    if len(range_list) == 1:
        range_list.append(range_list[0])
    return range_list
