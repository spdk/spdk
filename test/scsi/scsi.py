#!/usr/bin/env python

import os
import subprocess
import time

def vhost_stop():
    output = subprocess.check_output('ps ax | grep vhost | grep -v grep | awk \'{print $1}\'', shell=True)
    if output:
        for pid in output.split('\n'):
            if pid not in '':
                print "Stop vhost"
                os.system("sudo kill {}".format(pid))
                time.sleep(5)

def vhost_start():
    print "Start vhost"
    try:
        os.system("NRHUGE=4096 ~/spdk/scripts/setup.sh &")
        os.system("~/spdk/app/vhost/vhost -c scsi.conf &")
        time.sleep(5)
    except:
        print "Error: unable to start vhost"

def main():
    vhost_stop()
    try:
        vhost_start()
        os.system("python ./rpc/rpc_scsi.py ~/spdk/scripts/rpc.py")
        vhost_stop()
    except:
        print "Error: unable to start tests"

if __name__ == "__main__":
    main()