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
import time
import os
from subprocess import check_call, call, check_output, Popen, PIPE
from threading import Thread
import threading
from test_case import TestCase


class FileSystem_integrity:

    def __init__(self, filesystemtype, backendname):
        self.filetype = filesystemtype
        self.backend = backendname
        self.kernel_package_path = "/home/linux-config.tar.gz"
        print "The kernel package's path is ", self.kernel_package_path
        kernel = call("ls /home/linux-config.tar.gz", shell=True)
        if kernel != 0:
            print "Please add kernel source."
            time.sleep(300)
        self.iso_package_path = "/home/RHEL6.5-20131111.0-Server-x86_64-DVD1.iso"
        print "The iso package's path is ", self.iso_package_path
        iso = call(
            "ls /home/RHEL6.5-20131111.0-Server-x86_64-DVD1.iso",
            shell=True)
        if iso != 0:
            print "Please add iso source."
            time.sleep(300)
        call("rm -rf /home/devicedev*", shell=True)

    def getdevicepath(self):
        if self.backend == "nvme_direct":
            output = check_output("lsblk -l -o NAME", shell=True)
            devices = re.findall("(nvme[0-9]n1+)\n", output)
            self.device_paths = ['/dev/' + dev for dev in devices]
        if self.backend == "iscsi_nvme":
            output = check_output("iscsiadm -m session -P 3", shell=True)
            devices = re.findall("Attached scsi disk (sd[a-z]+)", output)
            time.sleep(10)
            self.device_paths = ['/dev/' + dev for dev in devices]

    def format_devices(self):
        print "the file system for the devices is: ", self.filetype
        self.new_dev_paths = []
        for dev_path in self.device_paths:
            if self.backend == "nvme_direct":
                dev_paths = (dev_path) + "p"
            if self.backend == "iscsi_nvme":
                dev_paths = dev_path
            cmd = "parted -s {} mklabel msdos".format(dev_path)
            check_call(cmd, shell=True)
            dev = (dev_path).lstrip('/dev/')

            optimal_io_size = check_output(
                "cat /sys/block/{}/queue/optimal_io_size".format(dev), shell=True)
            alignment_offset = check_output(
                "cat /sys/block/{}/alignment_offset".format(dev), shell=True)
            physical_block_size = check_output(
                "cat /sys/block/{}/queue/physical_block_size".format(dev), shell=True)
            optimal_io_size = int(optimal_io_size)
            alignment_offset = int(alignment_offset)
            physical_block_size = int(physical_block_size)

            sector_num = (optimal_io_size + alignment_offset) / \
                physical_block_size
            if sector_num == 0:
                sector_num = 2048
            sector_number = str(sector_num) + "s"

            cmd = 'parted -s {0} mkpart primary {1} 100% '.format(
                dev_path, sector_number)
            check_call(cmd, shell=True)

            new_dev_path = (dev_paths) + "1"

            if "btrfs" == self.filetype:
                call("mkfs.btrfs -f {}".format(new_dev_path), shell=True)

            if "ext4" == self.filetype:
                call("mkfs.ext4 -F  {}".format(new_dev_path), shell=True)

            if "xfs" == self.filetype:
                call("mkfs.xfs -f {}".format(new_dev_path), shell=True)

            self.new_dev_paths.append(new_dev_path)

    def compilekernel(self):
        retval = ""
        call("mkdir -p /home/devicedev00", shell=True)
        try:
            check_call(
                "mount -o rw {} /home/devicedev00".format(self.new_dev_paths[0]), shell=True)
        except:
            os._exit(1)
        check_call(
            "dd if={} of=/home/devicedev00/linux_package".format(self.kernel_package_path), shell=True)
        cmd = "cd /home/devicedev00/ && tar -xvf linux_package"
        tarpro = Popen(cmd, shell=True)
        rc = tarpro.wait()
        if rc != 0:
            retval = "tar command failed"
            return retval
        print "The kernel version is 4.3.3"
        cmd = "cd /home/devicedev00/linux-4.3.3"
        makepro = Popen(cmd, shell=True)
        rc = makepro.wait()
        if rc != 0:
            retval = "make command failed"
        else:
            retval = "All tests passed"
        call("umount /home/devicedev00", shell=True)
        call("rm -rf /home/devicedev00", shell=True)
        time.sleep(20)

        return retval

    def onlycompilekernel(self, count):
        time.sleep(10)
        self.getdevicepath()
        self.format_devices()
        retval = ""
        call("rm -rf /home/devicedev0*", shell=True)
        if len(self.new_dev_paths) < 4:
            number = len(self.new_dev_paths)
        else:
            number = 4
        for i in range(number):
            dir_name = "/home/devicedev0" + str(i) + "/"
            call("mkdir -p {}".format(dir_name), shell=True)
            try:
                call(
                    "mount {} {}".format(
                        self.new_dev_paths[i],
                        dir_name),
                    shell=True)
            except:
                os._exit(1)
            call("dd if={} of={}/linux_package".format(self.kernel_package_path,
                                                       dir_name), shell=True)
            call("cd {} && tar -xvf linux_package".format(dir_name), shell=True)
        for i in range(count):
            cmd = "cd /home/devicedev00/linux* && make clean && make -j 64 &"
            cmd1 = "cd /home/devicedev01/linux* && make clean && make -j 64 &"
            cmd2 = "cd /home/devicedev02/linux* && make clean && make -j 64 &"
            cmd3 = "cd /home/devicedev03/linux* && make clean && make -j 64"
            makepro = call(cmd, shell=True)
            makepro1 = call(cmd1, shell=True)
            makepro2 = call(cmd2, shell=True)
            time.sleep(60)
            makepro3 = call(cmd3, shell=True)
            if makepro != 0 or makepro1 != 0 or makepro2 != 0 or makepro3 != 0:
                retval = "make command failed"
                break
            else:
                retval = "All tests passed"
        time.sleep(30)
        call("umount /home/devicedev* &", shell=True)
        time.sleep(60)
        call("rm -rf /home/devicedev*", shell=True)
        time.sleep(20)

        return retval

    def run_threading_function(self, dev_path, number, thread_num):
        dev_name = dev_path
        dir_name = "/home/devicedev0" + str(number) + "/"
        if os.path.isdir(dir_name):
            pass
        else:
            call("mkdir -p {}".format(dir_name), shell=True)
            self.all_dir_name.append(dir_name)
        if int(self.current_loop) == 0 and 0 == int(thread_num):
            try:
                check_call(
                    "mount -o rw {} {}".format(dev_name, dir_name), shell=True)
            except:
                print "mount command run failed "
                self.fail_count = 1
                return "mount command fail"
        ddfile = dir_name + "newddfile-" + str(thread_num)
        try:
            check_call("touch {}".format(ddfile), shell=True)
        except:
            print "touch command failed"
            self.fail_count = 1
            return "touch comand failed"
        cmd = ' cd {0} && dd if={1} of={2} bs=1M'.format(
            dir_name, self.dd_file_path, ddfile)
        ddcmd = Popen(cmd, shell=True)
        rc = ddcmd.wait()
        if rc != 0:
            print "dd cmd run fail"
            self.fail_count = 1
            return "dd cmd failed"
        try:
            self.new_sha256_value = check_output(
                "sha256sum {}".format(ddfile), shell=True)
        except:
            print "sha256sum command failed"
            return "sha256sum command failed"
        self.new_sha256_value = re.split('[ ]+', self.new_sha256_value)[0]
        if self.sha256_value == self.new_sha256_value:
            pass
        else:
            print "sha256 comparition failed."
            self.fail_count += 1
            print self.new_sha256_value
            print self.sha256_value
        try:
            check_call("rm -rf {}".format(ddfile), shell=True)
        except:
            print "rm command failed"
            self.fail_count = 1
            return "rm command failed"

    def run_single_thread(self, dev_paths, index):
        all_dev_paths = dev_paths
        num_dev = len(all_dev_paths)
        for i in range(num_dev):
            thread1 = Thread(target=self.run_threading_function,
                             args=(all_dev_paths[i], i, index,))
            thread1.start()

        num1 = threading.activeCount()
        num1 = int(num1)

    def run_thread(self):
        file_count = 8
        if "large" == self.test_type:
            for i in range(file_count):
                self.run_single_thread(self.new_dev_paths, i)

                time.sleep(12)
                while True:
                    cmd = check_output("ps -C dd|wc -l", shell=True)
                    if 1 == int(cmd):
                        break
                    else:
                        continue
        else:
            for j in range(file_count):
                self.run_single_thread(self.new_dev_paths, j)
                time.sleep(1)

    def unmount_dir(self):
        print self.all_dir_name
        for dir_name in self.all_dir_name:
            call("umount {}".format(dir_name), shell=True)
        call("rm -rf /home/nvme*", shell=True)

    def run_the_cycles(self):
        self.run_thread()
        while True:
            num = threading.activeCount()

            if 1 == num:
                if int(self.current_loop) == (int(self.run_count) - 1):
                    self.unmount_dir()
                break

    def run_filesystem_integrity(self, run_count, test_type):
        self.run_count = int(run_count)
        self.current_loop = 90
        if test_type == 'large':
            self.dd_file_path = self.iso_package_path
        elif test_type == 'small':
            self.dd_file_path = self.kernel_package_path
        else:
            self.dd_file_path = self.kernel_package_path
        self.test_type = test_type
        time.sleep(10)
        self.sha256_value = check_output(
            "sha256sum {}".format(self.dd_file_path), shell=True)
        time.sleep(10)
        self.sha256_value = re.split('[ ]+', self.sha256_value)[0]
        self.fail_count = 0
        time.sleep(10)
        self.getdevicepath()
        self.format_devices()

        self.all_dir_name = []
        for i in range(self.run_count):
            self.current_loop = i
            self.run_the_cycles()
        print self.fail_count
        if self.fail_count != 0:
            print "some tests failed"
            return "some tests failed"
        else:
            retval = self.compilekernel()
            return retval
