import json
import sys
from uuid import UUID
from subprocess import check_output, CalledProcessError


class Spdk_Rpc(object):
    def __init__(self, rpc_py):
        self.rpc_py = rpc_py

    def __getattr__(self, name):
        def call(*args):
            cmd = "{} {} {}".format(sys.executable, self.rpc_py, name)
            for arg in args:
                cmd += " {}".format(arg)
            try:
                output = check_output(cmd, shell=True)
                return output.decode('ascii').rstrip('\n'), 0
            except CalledProcessError as e:
                print("ERROR: RPC Command {cmd} "
                      "execution failed:". format(cmd=cmd))
                print("Failed command output:")
                print(e.output)
                return e.output.decode('ascii'), e.returncode
        return call


class Commands_Rpc(object):
    def __init__(self, rpc_py):
        self.rpc = Spdk_Rpc(rpc_py)

    def check_bdev_get_bdevs_methods(self, uuid_bdev, bdev_size_mb, bdev_alias=""):
        print("INFO: Check RPC COMMAND bdev_get_bdevs")
        output = self.rpc.bdev_get_bdevs()[0]
        json_value = json.loads(output)
        for i in range(len(json_value)):
            uuid_json = json_value[i]['name']
            aliases = json_value[i]['aliases']

            if uuid_bdev in [uuid_json]:
                print("Info: UUID:{uuid} is found in RPC Command: "
                      "gets_bdevs response".format(uuid=uuid_bdev))
                # Check if human-friendly alias is as expected
                if bdev_alias and aliases:
                    if bdev_alias not in aliases:
                        print("ERROR: Expected bdev alias not found")
                        print("Expected: {name}".format(name=bdev_alias))
                        print("Actual: {aliases}".format(aliases=aliases))
                        return 1
                # num_block and block_size have values in bytes
                num_blocks = json_value[i]['num_blocks']
                block_size = json_value[i]['block_size']
                if num_blocks * block_size == bdev_size_mb * 1024 * 1024:
                    print("Info: Response bdev_get_bdevs command is "
                          "correct. Params: uuid_bdevs: {uuid}, bdev_size "
                          "{size}".format(uuid=uuid_bdev,
                                          size=bdev_size_mb))
                    return 0
        print("INFO: UUID:{uuid} or bdev_size:{bdev_size_mb} not found in "
              "RPC COMMAND bdev_get_bdevs: "
              "{json_value}".format(uuid=uuid_bdev, bdev_size_mb=bdev_size_mb,
                                    json_value=json_value))
        return 1

    def check_bdev_lvol_get_lvstores(self, base_name, uuid, cluster_size=None, lvs_name=""):
        print("INFO: RPC COMMAND bdev_lvol_get_lvstores")
        json_value = self.bdev_lvol_get_lvstores()
        if json_value:
            for i in range(len(json_value)):
                json_uuid = json_value[i]['uuid']
                json_cluster = json_value[i]['cluster_size']
                json_base_name = json_value[i]['base_bdev']
                json_name = json_value[i]['name']

                if base_name in json_base_name \
                        and uuid in json_uuid:
                    print("INFO: base_name:{base_name} is found in RPC "
                          "Command: bdev_lvol_get_lvstores "
                          "response".format(base_name=base_name))
                    print("INFO: UUID:{uuid} is found in RPC Command: "
                          "bdev_lvol_get_lvstores response".format(uuid=uuid))
                    if cluster_size:
                        if str(cluster_size) in str(json_cluster):
                            print("Info: Cluster size :{cluster_size} is found in RPC "
                                  "Command: bdev_lvol_get_lvstores "
                                  "response".format(cluster_size=cluster_size))
                        else:
                            print("ERROR: Wrong cluster size in lvol store")
                            print("Expected:".format(cluster_size))
                            print("Actual:".format(json_cluster))
                            return 1

                    # Also check name if param is provided:
                    if lvs_name:
                        if lvs_name not in json_name:
                            print("ERROR: Lvol store human-friendly name does not match")
                            print("Expected: {lvs_name}".format(lvs_name=lvs_name))
                            print("Actual: {name}".format(name=json_name))
                            return 1
                    return 0
            print("FAILED: UUID: lvol store {uuid} on base_bdev: "
                  "{base_name} not found in bdev_lvol_get_lvstores()".format(uuid=uuid,
                                                                             base_name=base_name))
            return 1
        else:
            print("INFO: Lvol store not exist")
            return 2
        return 0

    def bdev_malloc_create(self, total_size, block_size):
        print("INFO: RPC COMMAND bdev_malloc_create")
        output = self.rpc.bdev_malloc_create(total_size, block_size)[0]
        return output.rstrip('\n')

    def bdev_lvol_create_lvstore(self, base_name, lvs_name, cluster_size=None, clear_method=None):
        print("INFO: RPC COMMAND bdev_lvol_create_lvstore")
        if cluster_size:
            output = self.rpc.bdev_lvol_create_lvstore(base_name,
                                                       lvs_name,
                                                       "-c {cluster_sz}".format(cluster_sz=cluster_size))[0]
        elif clear_method:
            output = self.rpc.bdev_lvol_create_lvstore(base_name,
                                                       lvs_name,
                                                       "--clear-method {clear_m}".format(clear_m=clear_method))[0]
        else:
            output = self.rpc.bdev_lvol_create_lvstore(base_name, lvs_name)[0]
        return output.rstrip('\n')

    def bdev_lvol_create(self, uuid, lbd_name, size, thin=False):
        print("INFO: RPC COMMAND bdev_lvol_create")
        try:
            uuid_obj = UUID(uuid)
            name_opt = "-u"
        except ValueError:
            name_opt = "-l"
        thin_provisioned = ""
        if thin:
            thin_provisioned = "-t"
        output = self.rpc.bdev_lvol_create(name_opt, uuid, lbd_name, size, thin_provisioned)[0]
        return output.rstrip('\n')

    def bdev_lvol_delete_lvstore(self, uuid):
        print("INFO: RPC COMMAND bdev_lvol_delete_lvstore")
        try:
            uuid_obj = UUID(uuid)
            name_opt = "-u"
        except ValueError:
            name_opt = "-l"
        output, rc = self.rpc.bdev_lvol_delete_lvstore(name_opt, uuid)
        return rc

    def bdev_malloc_delete(self, base_name):
        print("INFO: RPC COMMAND bdev_malloc_delete")
        output, rc = self.rpc.bdev_malloc_delete(base_name)
        return rc

    def bdev_lvol_delete(self, bdev_name):
        print("INFO: RPC COMMAND bdev_lvol_delete")
        output, rc = self.rpc.bdev_lvol_delete(bdev_name)
        return rc

    def bdev_lvol_resize(self, uuid, new_size):
        print("INFO: RPC COMMAND bdev_lvol_resize")
        output, rc = self.rpc.bdev_lvol_resize(uuid, new_size)
        return rc

    def bdev_lvol_set_read_only(self, uuid):
        print("INFO: RPC COMMAND bdev_lvol_set_read_only")
        output, rc = self.rpc.bdev_lvol_set_read_only(uuid)
        return rc

    def nbd_start_disk(self, bdev_name, nbd_name):
        print("INFO: RPC COMMAND nbd_start_disk")
        output, rc = self.rpc.nbd_start_disk(bdev_name, nbd_name)
        return rc

    def nbd_stop_disk(self, nbd_name):
        print("INFO: RPC COMMAND nbd_stop_disk")
        output, rc = self.rpc.nbd_stop_disk(nbd_name)
        return rc

    def bdev_lvol_get_lvstores(self, name=None):
        print("INFO: RPC COMMAND bdev_lvol_get_lvstores")
        if name:
            output = json.loads(self.rpc.bdev_lvol_get_lvstores("-l", name)[0])
        else:
            output = json.loads(self.rpc.bdev_lvol_get_lvstores()[0])
        return output

    def get_lvol_bdevs(self):
        print("INFO: RPC COMMAND bdev_get_bdevs; lvol bdevs only")
        output = []
        rpc_output = json.loads(self.rpc.bdev_get_bdevs()[0])
        for bdev in rpc_output:
            if bdev["product_name"] == "Logical Volume":
                output.append(bdev)
        return output

    def get_lvol_bdev_with_name(self, name):
        print("INFO: RPC COMMAND bdev_get_bdevs; lvol bdevs only")
        rpc_output = json.loads(self.rpc.bdev_get_bdevs("-b", name)[0])
        if len(rpc_output) > 0:
            return rpc_output[0]

        return None

    def bdev_lvol_rename_lvstore(self, old_name, new_name):
        print("INFO: Renaming lvol store from {old} to {new}".format(old=old_name, new=new_name))
        output, rc = self.rpc.bdev_lvol_rename_lvstore(old_name, new_name)
        return rc

    def bdev_lvol_rename(self, old_name, new_name):
        print("INFO: Renaming lvol bdev from {old} to {new}".format(old=old_name, new=new_name))
        output, rc = self.rpc.bdev_lvol_rename(old_name, new_name)
        return rc

    def bdev_lvol_snapshot(self, bdev_name, snapshot_name):
        print("INFO: RPC COMMAND bdev_lvol_snapshot")
        output, rc = self.rpc.bdev_lvol_snapshot(bdev_name, snapshot_name)
        return rc

    def bdev_lvol_clone(self, snapshot_name, clone_name):
        print("INFO: RPC COMMAND bdev_lvol_clone")
        output, rc = self.rpc.bdev_lvol_clone(snapshot_name, clone_name)
        return rc

    def bdev_lvol_inflate(self, clone_name):
        print("INFO: RPC COMMAND bdev_lvol_inflate")
        output, rc = self.rpc.bdev_lvol_inflate(clone_name)
        return rc

    def bdev_lvol_decouple_parent(self, clone_name):
        print("INFO: RPC COMMAND bdev_lvol_decouple_parent")
        output, rc = self.rpc.bdev_lvol_decouple_parent(clone_name)
        return rc

    def bdev_aio_create(self, aio_path, aio_name, aio_bs=""):
        print("INFO: RPC COMMAND bdev_aio_create")
        output, rc = self.rpc.bdev_aio_create(aio_path, aio_name, aio_bs)
        return rc

    def bdev_aio_delete(self, aio_name):
        print("INFO: RPC COMMAND bdev_aio_delete")
        output, rc = self.rpc.bdev_aio_delete(aio_name)
        return rc
