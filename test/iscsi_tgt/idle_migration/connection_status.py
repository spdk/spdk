#!/usr/bin/env python

import json
import os
import sys
from time import sleep
from subprocess import check_output

rpc_py = os.path.dirname(os.path.realpath(__file__)) + '/../../../scripts/rpc.py'

class spdk_rpc(object):

    def __init__(self, rpc_py):
        self.rpc_py = rpc_py

    def __getattr__(self, name):
        def call(*args):
            cmd = "python {} {}".format(self.rpc_py, name)
            for arg in args:
                cmd += " {}".format(arg)
            return check_output(cmd, shell=True)
        return call

if __name__ == '__main__':

    if (len(sys.argv) < 2) or (sys.argv[1] != "idle" and sys.argv[1] != "active"):
        print "must specify \"idle\" or \"active\""
        sys.exit(1)

    rpc = spdk_rpc(rpc_py)

    idle = 0
    active = 0

    # capture connection state 10 times, 10 ms apart and keep a
    #  a running count of how many connections were found idle
    #  and active
    for i in range(10):

        conns = json.loads(rpc.get_iscsi_connections())
        num_conns = len(conns)

        for conn in conns:
            if conn['is_idle'] == 1:
                idle += 1
            else:
                active += 1

        # sleep 10ms
        sleep(0.01)

    active_pct = float(active) / (idle + active)

    # even when there is no active I/O on a connection, there could be
    #  a nopin/nopout being processed which causes a connection to
    #  temporarily go active;  also even when fio is actively running
    #  there could be a brief period of time where the initiator has
    #  no active I/O to some connection
    #
    # so do not enforce that *all* connections must be idle or active;
    #  allow for some percentage of anomalies
    anomaly_pct_allowed = 0.10

    print "active = {}".format(active)
    print "idle = {}".format(idle)
    print "active_pct = {}".format(active_pct)

    if sys.argv[1] == "idle" and active_pct > anomaly_pct_allowed:
        sys.exit(1)

    if sys.argv[1] == "active" and active_pct < (1.00 - anomaly_pct_allowed):
        sys.exit(1)

    sys.exit(0)
