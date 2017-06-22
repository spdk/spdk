 #!/usr/bin/env python

import os
import time
import sys


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


def vhost_start(dir_spdk, vhost_pid_path, config):

    print "Start vhost"
    try:
        os.system("{}/app/vhost/vhost -c {}{} & echo $! > {}".format(dir_spdk, dir_spdk, config, vhost_pid_path))
        time.sleep(5)

    except Exception as e:
        print "Error: unable to start vhost"
        raise e

    return vhost_pid_path


def main():

    test = ""
    config = ""

    dir_spdk = os.path.abspath('../../..')
    vhost_pid_path = "{}/vhost.pid".format(dir_spdk)

    if len(sys.argv) > 1:
        test = sys.argv[1]
        config = sys.argv[2]
    else:
        test = "test_base"
        config = "/test/vhost/rpc/configuration/scsi.conf"
    try:

        vhost_stop(vhost_pid_path)
        vhost_pid_path = vhost_start(dir_spdk, vhost_pid_path, config)
        if len(sys.argv) < 4:
            os.system("python {}/test/vhost/rpc/test/rpc_scsi_test.py {} {}/scripts/rpc.py".format(dir_spdk, test, dir_spdk))
        else:
            os.system("python {}test/vhost/rpc/test/rpc_scsi_test.py {}"
                      "{}/scripts/rpc.py {}".format(dir_spdk,test, dir_spdk,
                                                    sys.argv[3]))
        vhost_stop(vhost_pid_path)

    except Exception as e:
        print "Error: unable to start tests"
        raise e

if __name__ == "__main__":
    main()
