#!/usr/bin/env python


from subprocess import check_call, call, check_output, Popen, PIPE
import re
import sys
import signal
import time
import os


def get_target_devices(test_type):
    if test_type == 'nvmf':
        output = check_output('nvme list', shell=True)
        devices = re.findall("(nvme[0-9]+n[0-9]+).*SPDK bdev Controller", output)
    if test_type == 'iscsi':
        try:
            output = check_output('iscsiadm -m session -P 3', shell=True)
            devices = re.findall("Attached scsi disk (sd[a-z]+)", output)
        except:
            print "Could not find iSCSI disks."
            devices = []
    if not devices:
        print "No {} disks are found.".format(test_type)
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
    configure_devices(total_devs)
    devices_value = '[{}]'.format(','.join(total_devs))
    print devices_value


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
    get_target_devices(sys.argv[1])
