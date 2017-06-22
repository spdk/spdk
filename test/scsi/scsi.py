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
                os.system("kill -9 {}".format(pid))
                time.sleep(5)


def vhost_start(dir_spdk):
    print "Start vhost"
    try:
        os.system("NRHUGE=4096 {}/scripts/setup.sh &".format(dir_spdk))
        os.system("{}/app/vhost/vhost -c scsi.conf &".format(dir_spdk))
        time.sleep(5)

    except Exception as e:
        print "Error: unable to start vhost"
        raise e

def main():
    dir_spdk = os.path.abspath('../..')
    vhost_stop()
    try:
        vhost_start(dir_spdk)
        os.system("python ./rpc/rpc_scsi.py {}/scripts/rpc.py".format(dir_spdk))
        vhost_stop()

    except Exception as e:
        print "Error: unable to start tests"
        raise e

if __name__ == "__main__":
    main()
