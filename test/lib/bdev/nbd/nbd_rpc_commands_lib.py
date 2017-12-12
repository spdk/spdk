import json
from uuid import UUID
from subprocess import check_output, CalledProcessError


class Spdk_Rpc(object):
    def __init__(self, rpc_py):
        self.rpc_py = rpc_py

    def __getattr__(self, name):
        def call(*args):
            cmd = "python {} {}".format(self.rpc_py, name)
            for arg in args:
                cmd += " {}".format(arg)
            try:
                output = check_output(cmd, shell=True)
                return output.rstrip('\n'), 0
            except CalledProcessError as e:
                print("ERROR: RPC Command {cmd} "
                      "execution failed:". format(cmd=cmd))
                print("Failed command output:")
                print(e.output)
                return e.output, e.returncode
        return call


class Commands_Rpc(object):
    def __init__(self, rpc_py):
        self.rpc = Spdk_Rpc(rpc_py)

    def get_unclaimed_bdevs(self):
        print("INFO: Get unclaimed bdevs")
        unclaimed_bdevs = []
        output = self.rpc.get_bdevs()[0]
        json_value = json.loads(output)
        for i in range(len(json_value)):
            if not json_value[i]['claimed']:
                print("Info: bdev:{name} is found and unclaimed in RPC Command: "
                      "gets_bdevs response".format(name=json_value[i]['name']))
                unclaimed_bdevs += [json_value[i]['name']]
            else:
                print("Info: bdev:{name} is found but claimed in RPC Command: "
                      "gets_bdevs response".format(name=json_value[i]['name']))
        return unclaimed_bdevs

    def get_unused_nbds(self):
        print("INFO: Get unused nbds")
        # Get nbd list that nbd.mod supports
        cmd = "lsblk -a -d -n -o NAME | grep nbd"
        output = check_output(cmd, shell=True)
        all_nbds = output.rstrip(b'\n').split(b'\n')
        # Get system exported nbd list
        cmd = 'lsblk -d -n -o NAME | grep nbd || true'
        output = check_output(cmd, shell=True)
        used_nbds = output.rstrip(b'\n').split(b'\n')
        # Avail nbd list is the
        avail_nbds = list(set(all_nbds) - set(used_nbds))
        avail_nbds = ['/dev/' + i for i in avail_nbds]
        return avail_nbds

    def start_nbd_disks(self, unclaimed_bdevs, unused_nbds):
        print("INFO: Start unclaimed bdevs to be nbd devices")
        spdk_nbds = []
        rc = 0
        for i in range(len(unclaimed_bdevs)):
            if len(unused_nbds) > 0:
                nbd_name = unused_nbds[0]
                del(unused_nbds[0])
                print("Info: export bdev:{bdev} as nbd:{nbd} by RPC Command: "
                      "start_nbd_disk request".format(bdev=unclaimed_bdevs[i], nbd=nbd_name))
                output, rc = self.rpc.start_nbd_disk(unclaimed_bdevs[i], nbd_name)
                if rc != 0:
                    break
                else:
                    spdk_nbds += [nbd_name]
            else:
                print("Info: nbd devices are used out")
                break
        return spdk_nbds, rc

    def stop_nbd_disks(self, spdk_nbds):
        print("INFO: Stop exported nbd devices")
        rc = 0
        for i in range(len(spdk_nbds)):
            print("Info: Stop exported device nbd:{nbd}".format(nbd=spdk_nbds[i]))
            output, rc = self.rpc.stop_nbd_disk(spdk_nbds[i])
            if rc != 0:
                break
        return rc

    def get_nbd_disks(self, spdk_nbds):
        print("INFO: Get exported nbd devices")
        rcs = 0
        json_value = []
        if len(spdk_nbds) == 0:
            # get all nbd disks info
            output, rc = self.rpc.get_nbd_disks()
            json_value = json.loads(output)
            rcs = rc
        else:
            # get specific nbd disk info
            for i in range(len(spdk_nbds)):
                output, rc = self.rpc.get_nbd_disks("-n" + spdk_nbds[i])
                json_value += json.loads(output)
                rcs += rc

        for i in range(len(json_value)):
            bdev = json_value[i]['bdev_name']
            nbd = json_value[i]['nbd_device']
            print("Info: bdev:{bdev} is found as nbd:{nbd} in RPC Command: "
                  "get_nbd_disks response".format(bdev=bdev, nbd=nbd))
        return rcs
