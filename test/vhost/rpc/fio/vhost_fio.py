#!/usr/bin/env python

import glob
import os
from subprocess import Popen, PIPE
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
%(verify)s
verify_dump=1

"""

verify_template = """
do_verify=1
verify=meta
verify_pattern="meta"
"""


fio_job_template = """
[job%(jobnumber)d]
filename=%(device)s

"""


def interrupt_handler(signum, frame):
    fio.terminate()
    print "FIO terminated"
    sys.exit(0)


def main():

    global fio
    if len(sys.argv) < 5:
        print ("usage:")
        print ("  " + sys.argv[0] + " <io_size> <queue_depth> <test_type>"
                                    " <runtime>")
        print ("advanced usage:")
        print ("If you want to run fio with verify, please add verify string "
               "after runtime.")
        print ("Currently fio.py only support write rw randwrite randrw with"
               " verify enabled.")
        sys.exit(1)

    io_size = int(sys.argv[1])
    queue_depth = int(sys.argv[2])
    test_type = sys.argv[3]
    runtime = sys.argv[4]
    if len(sys.argv) > 5:
        verify = True
    else:
        verify = False

    devices = get_target_devices()
    print ("Found devices: ", devices)

    fio_executable = '/usr/bin/fio'

    device_paths = ['/dev/' + dev for dev in devices]
    print (device_paths)
    sys.stdout.flush()
    signal.signal(signal.SIGTERM, interrupt_handler)
    signal.signal(signal.SIGINT, interrupt_handler)
    fio = Popen([fio_executable, '-'], stdin=PIPE)
    fio.communicate(
        create_fio_config(io_size, queue_depth, device_paths, test_type,
                          runtime, verify))
    fio.stdin.close()
    rc = fio.wait()
    print ("FIO completed with code %d\n" % rc)
    sys.stdout.flush()
    sys.exit(rc)


def get_target_devices():
    for disk in glob.glob('/sys/class/block/*'):
        vendor_path = "{}/device/vendor".format(disk)
        if os.path.exists(vendor_path):
            if "INTEL" in open(vendor_path).read():
                return [(disk.split('/'))[-1]]


def create_fio_config(size, q_depth, devices, test, run_time, verify):
    if not verify:
        verifyfio = ""
    else:
        verifyfio = verify_template
    fiofile = fio_template % {"blocksize": size, "iodepth": q_depth,
                              "testtype": test, "runtime": run_time, "verify":
                                  verifyfio}

    for (i, dev) in enumerate(devices):
        fiofile += fio_job_template % {"jobnumber": i, "device": dev}
    return fiofile

if __name__ == "__main__":
    main()
