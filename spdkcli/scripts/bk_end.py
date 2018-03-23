import argparse
import sys
sys.path.append("../spdk/scripts")
import rpc.bdev as rpc_bdev
import rpc.client as rpc_client


class Node():
    def __init__(self):
        pass


class BKRoot(Node):

    current_bdevs = []
    # TODO: Allow using different socket and port number
    client = rpc_client.JSONRPCClient('/var/tmp/spdk.sock', 5260)

    @classmethod
    def list_bdevs(cls):
        args = argparse.Namespace(client=cls.client, name="")
        BKRoot.current_bdevs = rpc_bdev.get_bdevs(args)

    @classmethod
    def create_malloc_bdev(cls, **kwargs):
        args = argparse.Namespace(client=cls.client, **kwargs)

        # TODO: Need to catch failed RPC calls, modify rpc.client to do that!
        response = rpc_bdev.construct_malloc_bdev(args)
        args = argparse.Namespace(client=cls.client, name=response[0])
        new_bdev = rpc_bdev.get_bdevs(args)
        cls.current_bdevs.append(new_bdev[0])
        return new_bdev[0]["name"]

    @classmethod
    def get_bdevs(cls, bdev_type):
        for bdev in filter(lambda x: bdev_type in x["product_name"],
                           cls.current_bdevs):
            test = Bdev(bdev)
            yield test

    @classmethod
    def delete_bdev(cls, name):
        args = argparse.Namespace(client=cls.client, bdev_name=name)
        rpc_bdev.delete_bdev(args)
        cls.current_bdevs = filter(lambda b: name != b["name"],
                                   cls.current_bdevs)


class Bdev(Node):
    def __init__(self, bdev_info):
        """
        All class attributes are set based on what information is received
        from get_bdevs RPC call.
        # TODO: Document in docstring parameters which describe bdevs.
        # TODO: Possible improvement: JSON schema might be used here in future
        """
        for i in bdev_info.keys():
            setattr(self, i, bdev_info[i])
