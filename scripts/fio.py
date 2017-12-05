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

def main():

    global fio
    if (len(sys.argv) < 5):
        print "usage:"
        print "  " + sys.argv[0] + " <io_size> <queue_depth> <test_type> <runtime>"
        print "advanced usage:"
        print "  " + sys.argv[0] + " <io_size> <queue_depth> <test_type> <runtime> verify <runsize>"
        print "If you want to run fio with verify, please add verify string after runtime."
        print "You can also add runsize argument. The form of runsize is  [1-9]+{K, M, G}."
        print "For example, you can input 512K to just test 512K size of the disk."
        print "You can also input 512M or 128G for the runsize parameter."
        print "If you added verify argument and the test_type is read or randread, the"
        print "runsize argument is mandatory."
        sys.exit(1)

    io_size = int(sys.argv[1])
    queue_depth = int(sys.argv[2])
    test_type = sys.argv[3]
    runtime = sys.argv[4]
    if len(sys.argv) > 5:
        if sys.argv[5] == 'verify':
            verify = True
        else:
            verify = False
    else:
        verify = False
    if len(sys.argv) > 6:
        runsize = sys.argv[6]
    else:
        runsize = ''

    devices = get_target_devices()
    print "Found devices: ", devices

    configure_devices(devices)
    fio_executable = '/usr/bin/fio'

    device_paths = ['/dev/' + dev for dev in devices]
    sys.stdout.flush()
    signal.signal(signal.SIGTERM, interrupt_handler)
    signal.signal(signal.SIGINT, interrupt_handler)
    if verify and (test_type == "read" or test_type == "randread"):
        for device in device_paths:
            fio = Popen([fio_executable, '-'], stdin=PIPE)
            fio.communicate(create_fio_config(io_size, 1, [device], 'write', runtime, verify, runsize, True))
            fio.stdin.close()
            rc = fio.wait()
            if rc != 0:
                print "FIO write operaton completed with code %d\n" % rc
                sys.exit(rc)
    fio = Popen([fio_executable, '-'], stdin=PIPE)
    fio.communicate(create_fio_config(io_size, queue_depth, device_paths, test_type, runtime, verify, runsize, False))
    fio.stdin.close()
    rc = fio.wait()
    print "FIO completed with code %d\n" % rc
    sys.stdout.flush()
    sys.exit(rc)

def get_target_devices():
    output = check_output('lsblk -l -o NAME', shell=True)
    nvmf_devs = re.findall("(nvme[0-9]+n[0-9]+)\n", output)
    try:
        output = check_output('iscsiadm -m session -P 3', shell=True)
        iscsi_devs = re.findall("Attached scsi disk (sd[a-z]+)", output)
    except:
        print "Could not find iSCSI disks."
        iscsi_devs = []
    if not iscsi_devs and not nvmf_devs:
        print "No iSCSI disks and NVMF disks are found."
        sys.exit(0)
    return iscsi_devs + nvmf_devs

def create_fio_config(size, q_depth, devices, test, run_time, verify, run_size='', read_verify=False):
    run_time = time_template % {"runtime": run_time}
    if not verify:
        verifyfio = ""
        norandommap = 1
    elif not read_verify:
        verifyfio = verify_template
        norandommap = 0
    else:
        verifyfio = verify_write_template
        norandommap = 0
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

def set_device_parameter(device, filename_template, value):
    filename = filename_template % device
    f = open(filename, 'r+b')
    f.write(value)
    f.close()

def configure_devices(devices):
    for dev in devices:
        set_device_parameter(dev, "/sys/block/%s/queue/nomerges", "2")
        set_device_parameter(dev, "/sys/block/%s/queue/nr_requests", "128")
        requested_qd = 128
        qd = requested_qd
        while qd > 0:
            try:
                set_device_parameter(dev, "/sys/block/%s/device/queue_depth", str(qd))
                break
            except IOError:
                qd = qd - 1
        if qd == 0:
            print "Could not set block device queue depths."
        else:
            print "Requested queue_depth {} but only {} is supported.".format(str(requested_qd), str(qd))
        try:
            set_device_parameter(dev, "/sys/block/%s/queue/scheduler", "noop")
        except:
            print "Could not set block device queue scheduler."

if __name__ == "__main__":
    main()
