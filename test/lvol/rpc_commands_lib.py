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

    def check_get_bdevs_methods(self, uuid_bdev, bdev_size_mb):
        print("INFO: Check RPC COMMAND get_bdevs")
        output = self.rpc.get_bdevs()[0]
        json_value = json.loads(output)
        for i in range(len(json_value)):
            uuid_json = json_value[i]['name']
            if uuid_bdev in [uuid_json]:
                print("Info: UUID:{uuid} is found in RPC Command: "
                      "gets_bdevs response".format(uuid=uuid_bdev))
                # num_block and block_size have values in bytes
                num_blocks = json_value[i]['num_blocks']
                block_size = json_value[i]['block_size']
                if num_blocks * block_size == bdev_size_mb * 1024 * 1024:
                    print("Info: Response get_bdevs command is "
                          "correct. Params: uuid_bdevs: {uuid}, bdev_size "
                          "{size}".format(uuid=uuid_bdev,
                                          size=bdev_size_mb))
                    return 0
        print("INFO: UUID:{uuid} or bdev_size:{bdev_size_mb} not found in "
              "RPC COMMAND get_bdevs: "
              "{json_value}".format(uuid=uuid_bdev, bdev_size_mb=bdev_size_mb,
                                    json_value=json_value))
        return 1

    def check_get_lvol_stores(self, base_name, uuid, cluster_size=None):
        print("INFO: RPC COMMAND get_lvol_stores")
        json_value = self.get_lvol_stores()
        if json_value:
            for i in range(len(json_value)):
                json_uuid = json_value[i]['uuid']
                json_cluster = json_value[i]['cluster_size']
                json_base_name = json_value[i]['base_bdev']

                if base_name in json_base_name \
                        and uuid in json_uuid:
                    print("INFO: base_name:{base_name} is found in RPC "
                          "Command: get_lvol_stores "
                          "response".format(base_name=base_name))
                    print("INFO: UUID:{uuid} is found in RPC Command: "
                          "get_lvol_stores response".format(uuid=uuid))
                    if cluster_size:
                        if str(cluster_size) in str(json_cluster):
                            print("Info: Cluster size :{cluster_size} is found in RPC "
                                  "Command: get_lvol_stores "
                                  "response".format(cluster_size=cluster_size))
                        else:
                            print("ERROR: Wrong cluster size in lvol store")
                            print("Expected:".format(cluster_size))
                            print("Actual:".format(json_cluster))
                            return 1
                    return 0
            print("FAILED: UUID: lvol store {uuid} on base_bdev: "
                  "{base_name} not found in get_lvol_stores()".format(uuid=uuid,
                                                                      base_name=base_name))
            return 1
        else:
            print("INFO: Lvol store not exist")
            return 2
        return 0

    def construct_malloc_bdev(self, total_size, block_size):
        print("INFO: RPC COMMAND construct_malloc_bdev")
        output = self.rpc.construct_malloc_bdev(total_size, block_size)[0]
        return output.rstrip('\n')

    def construct_lvol_store(self, base_name, lvs_name, cluster_size):
        print("INFO: RPC COMMAND construct_lvol_store")
        output = self.rpc.construct_lvol_store(
            base_name,
            lvs_name,
            "-c {cluster_sz}".format(cluster_sz=cluster_size))[0]
        return output.rstrip('\n')

    def construct_lvol_bdev(self, uuid, lbd_name, size, thin=False):
        print("INFO: RPC COMMAND construct_lvol_bdev")
        try:
            uuid_obj = UUID(uuid)
            name_opt = "-u"
        except ValueError:
            name_opt = "-l"
        thin_provisioned = ""
        if thin:
            thin_provisioned = "-t"
        output = self.rpc.construct_lvol_bdev(name_opt, uuid, lbd_name, size, thin_provisioned)[0]
        return output.rstrip('\n')

    def destroy_lvol_store(self, uuid):
        print("INFO: RPC COMMAND destroy_lvol_store")
        try:
            uuid_obj = UUID(uuid)
            name_opt = "-u"
        except ValueError:
            name_opt = "-l"
        output, rc = self.rpc.destroy_lvol_store(name_opt, uuid)
        return rc

    def delete_bdev(self, base_name):
        print("INFO: RPC COMMAND delete_bdev")
        output, rc = self.rpc.delete_bdev(base_name)
        return rc

    def resize_lvol_bdev(self, uuid, new_size):
        print("INFO: RPC COMMAND resize_lvol_bdev")
        output, rc = self.rpc.resize_lvol_bdev(uuid, new_size)
        return rc

    def start_nbd_disk(self, bdev_name, nbd_name):
        print("INFO: RPC COMMAND start_nbd_disk")
        output, rc = self.rpc.start_nbd_disk(bdev_name, nbd_name)
        return rc

    def stop_nbd_disk(self, nbd_name):
        print("INFO: RPC COMMAND stop_nbd_disk")
        output, rc = self.rpc.stop_nbd_disk(nbd_name)
        return rc

    def get_lvol_stores(self, name=None):
        print("INFO: RPC COMMAND get_lvol_stores")
        if name:
            output = json.loads(self.rpc.get_lvol_stores("-l", name)[0])
        else:
            output = json.loads(self.rpc.get_lvol_stores()[0])
        return output

    def get_lvol_bdevs(self):
        print("INFO: RPC COMMAND get_bdevs; lvol bdevs only")
        output = []
        rpc_output = json.loads(self.rpc.get_bdevs()[0])
        for bdev in rpc_output:
            if bdev["product_name"] == "Logical Volume":
                output.append(bdev)
        return output

    def get_lvol_bdev_with_name(self, name):
        print("INFO: RPC COMMAND get_bdevs; lvol bdevs only")
        rpc_output = json.loads(self.rpc.get_bdevs("-b", name)[0])
        if len(rpc_output) > 0:
            return rpc_output[0]

        return None

    def construct_nvme_bdev(self, nvme_name, trtype, traddr):
        print("INFO: Add NVMe bdev {nvme}".format(nvme=nvme_name))
        self.rpc.construct_nvme_bdev("-b", nvme_name, "-t", trtype, "-a", traddr)

    def snapshot_lvol_bdev(self, bdev_name, snapshot_name):
        print("INFO: RPC COMMAND snapshot_lvol_bdev")
        output, rc = self.rpc.snapshot_lvol_bdev(bdev_name, snapshot_name)
        return rc

    def clone_lvol_bdev(self, snapshot_name, clone_name):
        print("INFO: RPC COMMAND clone_lvol_bdev")
        output, rc = self.rpc.clone_lvol_bdev(snapshot_name, clone_name)
        return rc
