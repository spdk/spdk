#!/usr/bin/env python

import os
import subprocess
import time


def check_pid_vhost():
    list_pid = (subprocess.check_output('ps ax | grep vhost | grep -v grep | awk \'{print $1}\'',
                                        shell=True)).split('\n')
    return list_pid[0]


def vhost_stop():
    pid = check_pid_vhost()
    if pid:
        print "Stop vhost"
        if os.system("kill -2 {}".format(pid)):
            print "Error: Vhost is not stopped.  Process IDentifier {}".format(pid)
        time.sleep(5)


def vhost_start(dir_spdk):
    print "Start vhost"
    try:
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
