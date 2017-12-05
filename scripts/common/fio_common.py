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
%(runtime)s
ioengine=libaio
direct=1
bs=%(blocksize)d
iodepth=%(iodepth)d
norandommap=%(norandommap)d
%(verify)s
verify_dump=1
%(runsize)s
"""

time_template = """
time_based=1
runtime=%(runtime)s
"""

verify_template = """
do_verify=1
verify=crc32c-intel
"""

verify_write_template = """
do_verify=0
verify=crc32c-intel
"""

runsize_template = """
size=%(runsize)s
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

    def parse_runsize_param(self, runsize):
        run_size = runsize
        if re.match("^[0-9]+(m|M|k|K|g|G)$", run_size):
            number = re.findall("([0-9]+)", run_size)
            if not number:
                print "runsize can't be zero."
                return
            return run_size
        else:
            print "Invalid runsize parameter input format."
            return

    def usage(self):
        self.script_name = str(self.test_type) + "_fio.py"
        print "usage:"
        print "  " + self.script_name + " <io_size> <queue_depth> <test_type> <runtime>"
        print "advanced usage:"
        print "If you want to run fio with verify, please add verify string after runtime."
        print "You can also add runsize argument. The form of runsize is  [1-9]+{k, K, m, M, g, G}."
        print "For example, you can input 512K to just test 512K size of the disk."
        print "You can also input 512M or 128G for the runsize parameter."
        print "If you added verify argument and the test_type is read or randread, the"
        print "runsize argument is mandatory."
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

    def create_fio_config(self, size, q_depth, devices, test, run_time, verify, run_size='', read_verify=False):
        run_time = time_template % {"runtime": run_time}
        norandommap = 0
        if not verify:
            verifyfio = ""
            norandommap = 1
        elif not read_verify:
            verifyfio = verify_template
        else:
            verifyfio = verify_write_template
        if run_size:
            runsizeparam = runsize_template % {"runsize": run_size}
            run_time = ""
        else:
            runsizeparam = ""
        fiofile = fio_template % {"blocksize": size, "iodepth": q_depth, "testtype": test, "runtime": run_time,
                                  "runsize": runsizeparam, "norandommap": norandommap, "verify": verifyfio}
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
        param = args[0]
        if len(param) < 4:
            self.usage()
        io_size = int(param[0])
        queue_depth = int(param[1])
        test_type = param[2]
        runtime = param[3]
        verify = True if len(param) > 4 else False
        runsize = self.parse_runsize_param(str(param[5])) if len(param) > 5 else ''

        devices = self.get_target_devices()
        print "Found devices: ", devices
        if self.test_type != 'nvmf':
            self.configure_devices(devices)

        fio_executable = '/usr/bin/fio'
        device_paths = ['/dev/' + dev for dev in devices]
        sys.stdout.flush()
        signal.signal(signal.SIGTERM, interrupt_handler)
        signal.signal(signal.SIGINT, interrupt_handler)
        if verify and (test_type == "read" or test_type == "randread"):
            for device in device_paths:
                fio = Popen([fio_executable, '-'], stdin=PIPE)
                fio.communicate(self.create_fio_config(io_size, 1, [device], 'write', runtime, verify, runsize, True))
                fio.stdin.close()
                rc = fio.wait()
                if rc != 0:
                    print "FIO write operaton completed with code {}\n".format(rc)
                    sys.exit(rc)
        fio = Popen([fio_executable, '-'], stdin=PIPE)
        fio.communicate(self.create_fio_config(io_size, queue_depth, device_paths, test_type, runtime, verify, runsize, False))
        fio.stdin.close()
        rc = fio.wait()
        print "FIO completed with code {}\n".format(rc)
        sys.stdout.flush()
        sys.exit(rc)
