 #!/usr/bin/env python

import os
import time


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
        os.system("{}/app/vhost/vhost -c scsi.conf & echo $! > {}".format(
                dir_spdk, vhost_pid_path))
        time.sleep(5)

    except Exception as e:
        print "Error: unable to start vhost"
        raise e

    return vhost_pid_path


def main():
    dir_spdk = os.path.abspath('../..')
    vhost_pid_path = "{}/vhost.pid".format(dir_spdk)
    try:
        vhost_stop(vhost_pid_path)
        vhost_pid_path = vhost_start(dir_spdk, vhost_pid_path)
        os.system("python ./rpc/rpc_scsi.py {}/scripts/rpc.py".format(
                dir_spdk))
        vhost_stop(vhost_pid_path)

    except Exception as e:
        print "Error: unable to start tests"
        raise e

if __name__ == "__main__":
    main()