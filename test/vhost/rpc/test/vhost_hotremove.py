#!/usr/bin/env python

import os
import subprocess

from verify_method_rpc import *


def exec_cmd(cmd, blocking):
    p = subprocess.Popen(cmd.split(" "), stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, stdin=subprocess.PIPE)
    if blocking is True:
        out, _ = p.communicate()
        return p.returncode, out
    return p


def copy_to_vm(vm_number, dir_vhost_path, fio):
    import pdb; pdb.set_trace()
    print("Running fio in vm: {0}".format(vm_number))

    exec_cmd("{dir_path}/fiotest/vm_ssh.sh {vm_num} sh -c \'rm "
             "vhost_fio.py\'".format(dir_path=dir_vhost_path,
                                     vm_num=vm_number), blocking=True)

    exec_cmd("{dir_path}/fiotest/vm_ssh.sh {vm_num} sh -c \'rm "
             "fio\'".format(dir_path=dir_vhost_path, vm_num=vm_number),
             blocking=True)

    # Copy FIO config to VM
    exec_cmd("cat {fio} | {dir_path}/fiotest/vm_ssh.sh {vm_num} \'cat > fio; "
             "chmod +x fio\'".format(dir_path=dir_vhost_path,
                                     vm_num=vm_number, fio=fio), blocking=True)

    exec_cmd("cat {dir_path}/rpc/fio/vhost_fio.py | "
             "{dir_path}/fiotest/vm_ssh.sh {vm_num} \'cat > vhost_fio.py; "
             "chmod +x vhost_fio.py\'".format(dir_path=dir_vhost_path,
                                              vm_num=vm_number), blocking=True)


def traffic_fio(dir_vhost_path, fio, output_path, vm_ip, vm_port, vm_number):

    copy_to_vm(vm_number, dir_vhost_path, fio)

    exec_cmd("{dir_path}/fiotest/vm_ssh.sh {vm_num} sh -c \'"
             "./vhost_fio.py 4096 128 randwrite 1 verify\'".format(
                dir_path=dir_vhost_path, vm_num=vm_number), blocking=True)

    # The fio.job file is auto creating in vhost_fio
    os.system("{fio} --output={output_path} "
              "--client={ip},{port}  --remote-config=fio.job".format(
                fio=fio, output_path=output_path, ip=vm_ip, port=vm_port))


def test_case(dir_vhost_path, fio, output_path, vp_ip, vm_port, vm_number,
              rpc_py, rpc_params):
    try:
        traffic_fio(dir_vhost_path, fio, output_path, vp_ip, vm_port,
                    vm_number)

        if not verify_remove_vhost_scsi_dev(rpc_py, rpc_params):
            print "Error, vhost_scsi_dev deleted dev"

        verify_get_bdevs_methods(rpc_py, rpc_params)

    except Exception as e:
        print "Error: unable to start tests"
        raise e


def main():
    # Variables for rpc test
    rpc_py = sys.argv[1]

    # Variables for traffic with fio
    fio = sys.argv[2]
    output_path = sys.argv[3]
    vp_ip = sys.argv[4]
    vm_port = sys.argv[5]
    vm_number = sys.argv[6]

    # Dictionary of variables for tests rpc
    rpc_params = {
        'scsi_devs': [],
        'ctrlr': unicode('scsi0'),
        'cpu_mask': unicode('0x1'),
        'scsi_dev_num': 0,
        'malloc_block_size': 4096
    }

    # The path to vhost file
    dir_vhost_path = os.path.abspath('../..')

    test_case(dir_vhost_path, fio, output_path, vp_ip, vm_port, vm_number,
              rpc_py, rpc_params)

if __name__ == "__main__":
    main()
