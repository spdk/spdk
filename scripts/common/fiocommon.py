#!/usr/bin/env python

from subprocess import check_call, call, check_output, Popen, PIPE
import re
import sys
import signal
import time
import os

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


class FioCommon(object):

    def __init__(self, test_type):
        self.test_type = test_type

    def usage(self):
        self.script_name = str(self.test_type) + "_fio.py"
        print "usage:"
        print "  " + self.script_name + " <device_list> <io_size> <queue_depth> <test_type> <runtime>"
        print "advanced usage:"
        print "If you want to run fio with verify, please add verify string after runtime."
        print "Currently fio.py only support write rw randwrite randrw with verify enabled."
        sys.exit(1)

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

    def run_fio(self, *args):
        global fio
        print len(args[0])
        if len(args[0]) < 5:
            self.usage()
        print args
        device_paths = ['/dev/' + dev for dev in args[0][0].split(',')]
        print device_paths
        io_size = int(args[0][1])
        queue_depth = int(args[0][2])
        test_type = args[0][3]
        runtime = args[0][4]
        verify = True if len(args[0]) > 5 else False

        fio_executable = 'fio'
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
