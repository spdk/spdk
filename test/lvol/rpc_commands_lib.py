import json
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
                print("Info: UUID:{uuid} is found in RPC Commnad: "
                      "gets_bdevs response".format(uuid=uuid_bdev))
                # num_block and block_size have values in bytes
                num_blocks = json_value[i]['num_blocks']
                block_size = json_value[i]['block_size']
                if num_blocks * block_size == bdev_size_mb * 1024 * 1024:
                    print("Info: Response get_bdevs command  is "
                          "correctly. Params: uuid_bdevs: {uuid}, bdev_size "
                          "{size}".format(uuid=uuid_bdev,
                                          size=bdev_size_mb))
                    return 0
        print("INFO: UUID:{uuid} or bdev_size:{bdev_size_mb} not found in "
              "RPC COMMAND get_bdevs: "
              "{json_value}".format(uuid=uuid_bdev, bdev_size_mb=bdev_size_mb,
                                    json_value=json_value))
        return 1

    def check_get_lvol_stores(self, base_name, uuid):
        print("INFO: RPC COMMAND get_lvol_stores")
        output = self.rpc.get_lvol_stores()[0]
        json_value = json.loads(output)
        if json_value:
            for i in range(len(json_value)):
                uuid_json_response = json_value[i]['uuid']
                base_bdev_json_reponse = json_value[i]['base_bdev']
                if base_name in [base_bdev_json_reponse] \
                        and uuid in [uuid_json_response]:
                    print("INFO: base_name:{base_name} is found in RPC "
                          "Command: get_lvol_stores "
                          "response".format(base_name=base_name))
                    print("INFO: UUID:{uuid} is found in RPC Commnad: "
                          "get_lvol_stores response".format(uuid=uuid))
                    return 0
            print("FAILED: UUID: {uuid} or base_name: {base_name} not found "
                  "in RPC COMMAND get_bdevs:"
                  "{json_value}".format(uuid=uuid, base_name=base_name,
                                        json_value=json_value))
            return 1
        else:
            print("INFO: Lvol store not exist")
        return 0

    def construct_malloc_bdev(self, total_size, block_size):
        print("INFO: RPC COMMAND construct_malloc_bdev")
        output = self.rpc.construct_malloc_bdev(total_size, block_size)[0]
        return output.rstrip('\n')

    def construct_lvol_store(self, base_name, size):
        print("INFO: RPC COMMAND construct_lvol_store")
        output = self.rpc.construct_lvol_store(base_name, "-c "
                                               "{size}".format(size=size))[0]
        return output.rstrip('\n')

    def construct_lvol_bdev(self, uuid, size):
        print("INFO: RPC COMMAND construct_lvol_bdev")
        output = self.rpc.construct_lvol_bdev(uuid, size)[0]
        return output.rstrip('\n')

    def destroy_lvol_store(self, uuid):
        print("INFO: RPC COMMAND destroy_lvol_store")
        output, rc = self.rpc.destroy_lvol_store(uuid)
        return rc

    def delete_bdev(self, base_name):
        print("INFO: RPC COMMAND delete_bdev")
        output, rc = self.rpc.delete_bdev(base_name)
        return rc

    def resize_lvol_bdev(self, uuid, new_size):
        print("INFO: RPC COMMAND resize_lvol_bdev")
        output, rc = self.rpc.resize_lvol_bdev(uuid, new_size)
        return rc

    def get_lvol_stores(self):
        print("INFO: RPC COMMAND get_lvol_stores")
        output = self.rpc.get_lvol_stores()[0]
        return output.rstrip('\n')
