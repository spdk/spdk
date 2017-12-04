#!/usr/bin/env python

from subprocess import check_call, call, check_output, Popen, PIPE
import re
import sys
import signal

fio_template = """
[global]
thread=1
invalidate=1
rw=%(testtype)s
time_based=1
runtime=%(runtime)s
ioengine=libaio
direct=1
bs=%(blocksize)d
iodepth=%(iodepth)d
norandommap=%(norandommap)d
%(verify)s
verify_dump=1

"""

verify_template = """
do_verify=1
verify=crc32c-intel
"""


fio_job_template = """
[job%(jobnumber)d]
filename=%(device)s

"""

def interrupt_handler(signum, frame):
    fio.terminate()
    print "FIO terminated"
    sys.exit(0)

class fio_common:

    def __init__(self, test_type):
        self.test_type = test_type

    def usage(self):
        self.script_name = str(self.test_type) + "_fio.py"
        print "usage:"
        print "  " + self.script_name + " <io_size> <queue_depth> <test_type> <runtime>"
        print "advanced usage:"
        print "If you want to run fio with verify, please add verify string after runtime."
        print "Currently fio.py only support write rw randwrite randrw with verify enabled."
        sys.exit(1)

    def get_target_devices(self):
        if self.test_type == 'nvmf':
            output = check_output('lsblk -l -o NAME', shell=True)
            devices = re.findall("(nvme[0-9]+n[0-9]+)\n", output)
        if self.test_type == 'iscsi':
            try:
                output = check_output('iscsiadm -m session -P 3', shell=True)
                devices = re.findall("Attached scsi disk (sd[a-z]+)", output)
            except:
                print "Could not find iSCSI disks."
                devices = []
        if not devices:
            print "No {} disks are found.".format(self.test_type)
            sys.exit(0)
        total_devs = []
        for dev in devices:
            output = check_output('lsblk /dev/{} -o MOUNTPOINT -n'.format(dev), shell=True)
            if re.search("\S", output):
                print "Active mountpoints on /dev/{}, so skip this device.".format(dev)
            else:
                total_devs += [dev]
        if not total_devs:
            print "All devices are mounted."
            sys.exit(0)
        return total_devs

    def create_fio_config(self, size, q_depth, devices, test, run_time, verify):
        norandommap = 0
        if not verify:
            verifyfio = ""
            norandommap = 1
        else:
            verifyfio = verify_template
        fiofile = fio_template % {"blocksize": size, "iodepth": q_depth, "testtype": test, "runtime": run_time,
                                  "norandommap": norandommap, "verify": verifyfio}
        for (i, dev) in enumerate(devices):
            fiofile += fio_job_template % {"jobnumber": i, "device": dev}
        return fiofile

    def set_device_parameter(self, device, filename_template, value):
        filename = filename_template % device
        f = open(filename, 'r+b')
        f.write(value)
        f.close()

    def configure_devices(self, devices):
        for dev in devices:
            self.set_device_parameter(dev, "/sys/block/%s/queue/nomerges", "2")
            self.set_device_parameter(dev, "/sys/block/%s/queue/nr_requests", "128")
            requested_qd = 128
            qd = requested_qd
            while qd > 0:
                try:
                    self.set_device_parameter(dev, "/sys/block/%s/device/queue_depth", str(qd))
                    break
                except IOError:
                    qd = qd - 1
            if qd == 0:
                print "Could not set block device queue depths."
            else:
                print "Requested queue_depth {} but only {} is supported.".format(str(requested_qd), str(qd))
            try:
                self.set_device_parameter(dev, "/sys/block/%s/queue/scheduler", "noop")
            except:
                print "Could not set block device queue scheduler."

    def run_fio(self, *args):
        global fio
        if len(args[0]) < 4:
            self.usage()
        io_size = int(args[0][0])
        queue_depth = int(args[0][1])
        test_type = args[0][2]
        runtime = args[0][3]
        verify = True if len(args) > 4 else False

        devices = self.get_target_devices()
        print "Found devices: ", devices
        if self.test_type != 'nvmf':
            self.configure_devices(devices)

        fio_executable = '/usr/bin/fio'
        device_paths = ['/dev/' + dev for dev in devices]
        sys.stdout.flush()
        signal.signal(signal.SIGTERM, interrupt_handler)
        signal.signal(signal.SIGINT, interrupt_handler)
        fio = Popen([fio_executable, '-'], stdin=PIPE)
        fio.communicate(self.create_fio_config(io_size, queue_depth, device_paths, test_type, runtime, verify))
        fio.stdin.close()
        rc = fio.wait()
        print "FIO completed with code {}\n".format(rc)
        sys.stdout.flush()
        sys.exit(rc)
