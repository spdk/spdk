#!/usr/bin/env python3

from subprocess import check_call, call, check_output, Popen, PIPE, CalledProcessError
import re
import sys
import signal
import os.path
import time
import argparse

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
numjobs=%(numjobs)s
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


def main(io_size, protocol, queue_depth, test_type, runtime, num_jobs, verify):
    global fio

    if protocol == "nvmf":
        devices = get_nvmf_target_devices()
    elif protocol == "iscsi":
        devices = get_iscsi_target_devices()

    configure_devices(devices)
    try:
        fio_executable = check_output("which fio", shell=True).split()[0]
    except CalledProcessError as e:
        sys.stderr.write(str(e))
        sys.stderr.write("\nCan't find the fio binary, please install it.\n")
        sys.exit(1)

    device_paths = ['/dev/' + dev for dev in devices]
    print("Device paths:")
    print(device_paths)
    sys.stdout.flush()
    signal.signal(signal.SIGTERM, interrupt_handler)
    signal.signal(signal.SIGINT, interrupt_handler)
    fio = Popen([fio_executable, '-'], stdin=PIPE)
    fio.communicate(create_fio_config(io_size, queue_depth, device_paths, test_type, runtime, num_jobs, verify).encode())
    fio.stdin.close()
    rc = fio.wait()
    print("FIO completed with code %d\n" % rc)
    sys.stdout.flush()
    sys.exit(rc)


def get_iscsi_target_devices():
    output = check_output('iscsiadm -m session -P 3', shell=True)
    return re.findall("Attached scsi disk (sd[a-z]+)", output.decode("ascii"))


def get_nvmf_target_devices():
    output = str(check_output('lsblk -l -o NAME', shell=True).decode())
    return re.findall("(nvme[0-9]+n[0-9]+)\n", output)


def create_fio_config(size, q_depth, devices, test, run_time, num_jobs, verify):
    norandommap = 0
    if not verify:
        verifyfio = ""
        norandommap = 1
    else:
        verifyfio = verify_template
    fiofile = fio_template % {"blocksize": size, "iodepth": q_depth,
                              "testtype": test, "runtime": run_time,
                              "norandommap": norandommap, "verify": verifyfio,
                              "numjobs": num_jobs}
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

    for dev in devices:
        retry = 30
        while retry > 0:
            if os.path.exists("/sys/block/%s/queue/nomerges" % dev):
                break
            else:
                retry = retry - 1
                time.sleep(0.1)

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
    elif qd < requested_qd:
        print("Requested queue_depth {} but only {} is supported.".format(str(requested_qd), str(qd)))
    if not set_device_parameter(devices, "/sys/block/%s/queue/scheduler", "noop"):
        set_device_parameter(devices, "/sys/block/%s/queue/scheduler", "none")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="fio.py")
    parser.add_argument("-i", "--io-size", type=int, help="The desired I/O size in bytes.", required=True)
    parser.add_argument("-p", "--protocol", type=str, help="The protocol we are testing against. One of iscsi or nvmf.", required=True)
    parser.add_argument("-d", "--queue-depth", type=int, help="The desired queue depth for each job.", required=True)
    parser.add_argument("-t", "--test-type", type=str, help="The fio I/O pattern to run. e.g. read, randwrite, randrw.", required=True)
    parser.add_argument("-r", "--runtime", type=int, help="Time in seconds to run the workload.", required=True)
    parser.add_argument("-n", "--num-jobs", type=int, help="The number of fio jobs to run in your workload. default 1.", default=1)
    parser.add_argument("-v", "--verify", action="store_true", help="Supply this argument to verify the I/O.", default=False)
    args = parser.parse_args()

    if args.protocol.lower() != "nvmf" and args.protocol.lower() != "iscsi":
        parser.error("Protocol must be one of the following: nvmf, iscsi.")

    main(args.io_size, args.protocol, args.queue_depth, args.test_type, args.runtime, args.num_jobs, args.verify)
