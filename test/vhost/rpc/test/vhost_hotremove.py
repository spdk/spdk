#!/usr/bin/env python

from os import *
from verify_method_rpc import *


def exec_cmd(cmd, blocking):
    p = subprocess.Popen(cmd.split(" "), stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, stdin=subprocess.PIPE)
    if blocking is True:
        out, _ = p.communicate()
        return p.returncode, out
    return p

def traffic_fio():
    fio = os.path.abspath('fio/vhost_fio.py')
    exec_cmd("python {}".format(fio), blocking=True)

def traffic_dd():
    pass
    #todo

def main():
    rpc_py = sys.argv[1]
    try:
        traffic_fio()
        Verify_rpc_method.verify_remove_vhost_scsi_dev(rpc_py, (
            {'ctrlr': unicode('scsi0')}))
        Verify_rpc_method.verify_get_bdevs_methods(rpc_py)
        Verify_rpc_method.verify_get_vhost_scsi_controllers()
        
        #check get_bdev
        #lsblk -> vm
        traffic_fio()
            #* brak lsblk
    except Exception as e:
        print "Error: unable to start tests"
        raise e

if __name__ == "__main__":
    main()






