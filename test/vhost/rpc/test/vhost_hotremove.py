#!/usr/bin/env python

import subprocess
from os import path
import os
from verify_method_rpc import *


def exec_cmd(cmd, blocking):
    p = subprocess.Popen(cmd.split(" "), stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, stdin=subprocess.PIPE)
    if blocking is True:
        out, _ = p.communicate()
        return p.returncode, out
    return p


def traffic_fio():
    fio = path.abspath('../fio/vhost_fio.py 4096 128 randwrite 1 verify')
    import pdb; pdb.set_trace()
    # exec_cmd("python {}".format(fio), blocking=True)
    os.system("python {}".format(fio))


def traffic_dd():
    pass
    # todo


def main():
    rpc_py = sys.argv[1]
    dev = sys.argv[2]
    rpc_params = {
        'scsi_devs': [],
        'ctrlr': unicode('scsi0'),
        'cpu_mask': unicode('0x1'),
        'scsi_dev_num': 0,
        'malloc_block_size': 4096
    }
    try:
        traffic_fio()

        verify_remove_vhost_scsi_dev(rpc_py, rpc_params)
        verify_get_bdevs_methods(rpc_py, rpc_params)
        verify_get_vhost_scsi_controllers(rpc_py, rpc_params, dev)
        
        traffic_fio()
            #* brak lsblk
    except Exception as e:
        print "Error: unable to start tests"
        raise e

if __name__ == "__main__":
    main()

