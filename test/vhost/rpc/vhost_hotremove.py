#!/usr/bin/env python

import time
from os import *

def check_pid_vhost(vhost_pid_path):
    if os.path.isfile(vhost_pid_path):
        with open(vhost_pid_path, 'r') as f:
            pid = f.read()
            f.close()
        return int(pid.rstrip('\n'))


def vhost_stop(vhost_pid_path):
    pid = check_pid_vhost(vhost_pid_path)
    if pid:
        print "Stop vhost"
        if os.kill(pid, 2):
            print "Error: Vhost is not stopped. Process IDentifier " \
                  "{}".format(pid)
        else:
            os.unlink(vhost_pid_path)


def vhost_start(dir_spdk, vhost_pid_path):

    print "Start vhost"
    try:
        os.system("{}/app/vhost/vhost -c {}/test/vhost/rpc/fio/hotremove.conf "
                  "& echo $! > {}".format(dir_spdk, dir_spdk, vhost_pid_path))
        os.system("{}/test/scsi/vm/vm_run.sh")
        time.sleep(5)

    except Exception as e:
        print "Error: unable to start vhost"
        raise e

    return vhost_pid_path

def exec_cmd(cmd, blocking):
    p = subprocess.Popen(cmd.split(" "), stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, stdin=subprocess.PIPE)
    if blocking is True:
        out, _ = p.communicate()
        return p.returncode, out
    return p

def create_folder(dir_user):
    vm_path =  "/vms/hotremove"
    try:
        if not os.path.isdir("{}{}".format(dir_user, vm_path)):
            os.makedirs("{}{}".format(dir_user, vm_path))
            qemu_file = open("{}{}/qemu.pid".format(dir_user, vm_path), 'w')
            qemu_file.close()
        elif os.path.isfile("{}{}/qemu.pid".format(dir_user, vm_path)):
            pass
    except Exception as e:
        print "Error: The folder doesn't created in the location"
        raise e

def vm_setup(dir_fiotest, vm_number):


    exec_cmd("{}/vm_setup.sh -f {} --test-type=spdk_vhost "
             "--os=fedora-25.qcow2".format(dir_fiotest, vm_number),
             blocking=True)

def vm_run(dir_fiotest,vm_number):

    exec_cmd("{../vm_run.sh {}".format(vm_number), blocking=True)

def remove_vhost_scsi_dev(dir_spdk):
    rpc = "python {}/scripts/rpc.py remove_vhost_scsi_dev scsi0 0".format(
            dir_spdk)
    exec_cmd(rpc, blocking=True)
    print("remove_vhost_scsi_dev_methods passed")

def traffic_fio():
    fio = os.path.abspath('fio/vhost_fio.py')
    exec_cmd("python {}".format(fio), blocking=True)
def traffic_dd():
    pass
    #todo
def main():
    dir_fiotest = os.path.abspath('../../fiotest')
    dir_user = os.path.abspath('../../../../..')
    dir_spdk = os.path.abspath('../../../..')
    vhost_pid_path = "{}/vhost.pid".format(dir_spdk)
    vm_number = 99
    try:
        vhost_stop(vhost_pid_path)
        vhost_pid_path = vhost_start(dir_spdk, vhost_pid_path)

        vm_setup(dir_fiotest, vm_number)
        vm_run(dir_fiotest, vm_number)
        # vm_ssh copy fio_bin todo

        traffic_fio()
        remove_vhost_scsi_dev(dir_spdk)
        traffic_fio()
        vhost_stop(vhost_pid_path)
        #vm_stop() todo

    except Exception as e:
        print "Error: unable to start tests"
        raise e

if __name__ == "__main__":
    main()






