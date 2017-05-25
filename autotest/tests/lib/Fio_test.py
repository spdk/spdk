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
rwmixread=%(rw_mixread)d
ioengine=libaio
bs=%(blocksize)d
direct=1
size=%(size)s
iodepth=%(iodepth)d
norandommap=1
%(verify)s
verify_dump=1
numjobs=1
%(trim)s
"""

fio_job_template = """
[job%(jobnumber)d]
filename=%(device)s
"""

verify_template = """
do_verify=1
verify=pattern
verify_pattern="spdk"
"""

verify_write_template = """
[global]
thread=1
invalidate=1
rw=%(testtype)s
ioengine=libaio
bs=%(blocksize)d
iodepth=%(iodepth)d
norandommap=1
direct=1
size=%(size)s
verify_dump=1
numjobs=1
do_verify=0
verify=pattern
verify_pattern="spdk"
"""

trim_template = """
trim_percentage=10
trim_verify_zero=1

"""


def start_fio_client():
    get_fio_version()
    start_time = datetime.datetime.now()
    print "start_time is ", start_time
    path = os.path.realpath(__file__)
    current_path = os.path.dirname(path)
    config = ConfigParser.ConfigParser()
    file_name = current_path + '/Fio_test.conf'
    config.readfp(open(file_name))
    target_ip = config.get("target", "nvmf_addr")
    target = xmlrpclib.ServerProxy(
        config.get('target', 'nvmf_testserver_addr'))
    devices_nvme = get_lsblk()
    print "Found devices: ", devices_nvme
    time.sleep(2)
    io_range = config.get("test", "io_size")
    queue_range = config.get("test", "queue_depth")
    test_type = config.get("test", "test_types")
    run_size = config.get("test", "runsize")
    verify = config.get("test", "verify")
    devices = get_lsblk()
    io_range_list = get_range(io_range)
    queue_range_list = get_range(queue_range)
    io_sizes = list(power_of_2_range(io_range_list[0], io_range_list[1]))
    queue_depths = list(power_of_2_range(
        queue_range_list[0], queue_range_list[1]))
    fio_executable = '/usr/bin/fio'
    device_paths = ['/dev/' + dev for dev in devices]
    sys.stdout.flush()
    if verify == "False":
        verify = False
    else:
        verify = True
    rwmixread = config.get('test', 'rwmixread')
    rwmixread = int(rwmixread)
    log = ""
    for io_size in io_sizes:
        for depth in queue_depths:
            if verify:
                if test_type == "read" or test_type == "randread":
                    for singledevice in device_paths:
                        singledevice = [singledevice]
                        fio = Popen([fio_executable, '-'], stdin=PIPE)
                        write_depth = 1
                        fio.communicate(create_verify_fio_config(
                            io_size, int(write_depth), singledevice, 'write', run_size))
                        fio.stdin.close()
                        rc = fio.wait()
                        print "FIO write operation completed with code %d\n" % rc
            time.sleep(3)
            sys.stdout.flush()
            fio = Popen([fio_executable, '-'], stdin=PIPE)
            fio.communicate(
                create_fio_config(
                    io_size,
                    depth,
                    device_paths,
                    test_type,
                    run_size,
                    verify,
                    rwmixread))
            fio.stdin.close()
            rc = fio.wait()
            print "FIO completed with code %d\n" % rc
            sys.stdout.flush()
            if rc != 0:
                log += "Failed %s at Size %d, queue depth %d\n" % (
                    test_type, io_size, depth)
    if len(log) == 0:
        print "All tests passed"
        return "All tests passed"
    else:
        print log
        return log


def create_fio_config(size, q_depth, devices, test,
                      run_size, rwmixread, verify=False):
    if not verify:
        verifyfio = ""
    else:
        verifyfio = verify_template
    if test == "trim" or test == "randtrim":
        trim_tem = trim_template
    else:
        trim_tem = ""
    fiofile = fio_template % {
        "blocksize": size,
        "iodepth": q_depth,
        "testtype": test,
        "rw_mixread": rwmixread,
        "verify": verifyfio,
        "trim": trim_tem,
        "size": run_size}
    for (i, dev) in enumerate(devices):
        fiofile += fio_job_template % {"jobnumber": i, "device": dev}
    return fiofile


def create_verify_fio_config(size, q_depth, devices, test, run_size):
    fiofile = verify_write_template % {
        "blocksize": size, "iodepth": q_depth, "testtype": test, "size": run_size}
    for (i, dev) in enumerate(devices):
        fiofile += fio_job_template % {"jobnumber": i, "device": dev}
    return fiofile


def set_device_parameter(devices, filename_template, value):
    for dev in devices:
        filename = filename_template % dev
        f = open(filename, 'r+b')
        f.write(value)
        f.close()


def configure_devices(devices):
    set_device_parameter(devices, "/sys/block/%s/queue/nomerges", "2")
    set_device_parameter(devices, "/sys/block/%s/queue/nr_requests", "128")
    set_device_parameter(devices, "/sys/block/%s/queue/scheduler", "noop")


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
        print "fio version must be 2.1.14 or above. Your fio version is ", versionstr
        sys.exit(1)


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


def get_range(item):
    range_string = item.split('-')
    range_list = [x.strip() for x in range_string]
    if len(range_list) == 1:
        range_list.append(range_list[0])
    return range_list


def get_lsblk():
    lsblk_log = check_output("lsblk -l -o NAME", shell=True)
    return re.findall("(nvme[0-9]+n1+)\n", lsblk_log)
