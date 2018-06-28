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
    print("FIO terminated")
    sys.exit(0)


def main():

    global fio
    if (len(sys.argv) < 5):
        print("usage:")
        print("  " + sys.argv[0] + " <io_size> <queue_depth> <test_type> <runtime>")
        print("advanced usage:")
        print("If you want to run fio with verify, please add verify string after runtime.")
        print("Currently fio.py only support write rw randwrite randrw with verify enabled.")
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
    print("Found devices: ", devices)

    configure_devices(devices)
    fio_executable = '/usr/bin/fio'

    device_paths = ['/dev/' + dev for dev in devices]
    sys.stdout.flush()
    signal.signal(signal.SIGTERM, interrupt_handler)
    signal.signal(signal.SIGINT, interrupt_handler)
    fio = Popen([fio_executable, '-'], stdin=PIPE)
    fio.communicate(create_fio_config(io_size, queue_depth, device_paths, test_type, runtime, verify).encode())
    fio.stdin.close()
    rc = fio.wait()
    print("FIO completed with code %d\n" % rc)
    sys.stdout.flush()
    sys.exit(rc)


def get_target_devices():
    output = check_output('iscsiadm -m session -P 3', shell=True)
    return re.findall("Attached scsi disk (sd[a-z]+)", output.decode("ascii"))


def create_fio_config(size, q_depth, devices, test, run_time, verify):
    norandommap = 0
    if not verify:
        verifyfio = ""
        norandommap = 1
    else:
        verifyfio = verify_template
    fiofile = fio_template % {"blocksize": size, "iodepth": q_depth,
                              "testtype": test, "runtime": run_time,
                              "norandommap": norandommap, "verify": verifyfio}
    for (i, dev) in enumerate(devices):
        fiofile += fio_job_template % {"jobnumber": i, "device": dev}
    return fiofile


def set_device_parameter(devices, filename_template, value):
    valid_value = True

    for dev in devices:
        filename = filename_template % dev
        f = open(filename, 'r+b')
        try:
            f.write(value.encode())
            f.close()
        except OSError:
            valid_value = False
            continue

    return valid_value


def configure_devices(devices):
    set_device_parameter(devices, "/sys/block/%s/queue/nomerges", "2")
    set_device_parameter(devices, "/sys/block/%s/queue/nr_requests", "128")
    requested_qd = 128
    qd = requested_qd
    while qd > 0:
        try:
            set_device_parameter(devices, "/sys/block/%s/device/queue_depth", str(qd))
            break
        except IOError:
            qd = qd - 1
    if qd == 0:
        print("Could not set block device queue depths.")
    else:
        print("Requested queue_depth {} but only {} is supported.".format(str(requested_qd), str(qd)))
    if not set_device_parameter(devices, "/sys/block/%s/queue/scheduler", "noop"):
        set_device_parameter(devices, "/sys/block/%s/queue/scheduler", "none")


if __name__ == "__main__":
    main()
