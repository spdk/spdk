#!/usr/bin/env python

import glob
import os
import sys

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


def main():

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

    device_paths = ['/dev/' + dev for dev in devices]
    print (device_paths)

    create_fio_config(io_size, queue_depth, device_paths, test_type,
                      runtime, verify)

    os.system("fio --daemonize=/tmp/fio.pid --server")


def get_target_devices():
    for disk in glob.glob('/sys/class/block/*'):
        vendor_path = "{}/device/vendor".format(disk)
        if os.path.exists(vendor_path):
            if "INTEL" in open(vendor_path).read():
                return [os.path.basename(disk)]


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
    with open("fio.job", 'w') as fio:
        fio.write(fiofile)

    fio.close()

if __name__ == "__main__":
    main()
