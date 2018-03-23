import argparse
import sys
sys.path.append("../spdk/scripts")
import rpc.bdev as rpc_bdev
import rpc.lvol as rpc_lvol
import rpc.client as rpc_client


class BKRoot():

    current_bdevs = []
    current_lvol_stores = []

    @classmethod
    def set_rpc_target(cls, s, p):
        cls.client = rpc_client.JSONRPCClient(s, p)

    @classmethod
    def list_bdevs(cls):
        args = argparse.Namespace(client=cls.client, name="")
        BKRoot.current_bdevs = rpc_bdev.get_bdevs(args)

    @classmethod
    def list_lvols(cls):
        args = argparse.Namespace(client=cls.client, lvs_name="", uuid="")
        BKRoot.current_lvol_stores = rpc_lvol.get_lvol_stores(args)

    @classmethod
    def get_lvol_stores(cls):
        for lvs in cls.current_lvol_stores:
            yield LvolStore(lvs)

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
    def create_aio_bdev(cls, **kwargs):
        args = argparse.Namespace(client=cls.client, **kwargs)

        # TODO: Need to catch failed RPC calls, modify rpc.client to do that!
        response = rpc_bdev.construct_aio_bdev(args)
        args = argparse.Namespace(client=cls.client, name=response[0])
        new_bdev = rpc_bdev.get_bdevs(args)
        cls.current_bdevs.append(new_bdev[0])
        return new_bdev[0]["name"]

    @classmethod
    def create_lvol_bdev(cls, **kwargs):
        args = argparse.Namespace(client=cls.client, **kwargs)
        # TODO: Need to catch failed RPC calls, modify rpc.client to do that!
        response = rpc_lvol.construct_lvol_bdev(args)
        args = argparse.Namespace(client=cls.client, name=response[0])
        new_bdev = rpc_bdev.get_bdevs(args)
        cls.current_bdevs.append(new_bdev[0])
        return new_bdev[0]["aliases"][0], new_bdev[0]["name"]

    @classmethod
    def get_bdevs(cls, bdev_type):
        # Following replace needs to be don in order for some of the bdev listings to work.
        # For example logical volumes: listing in menu is "Logical_Volume" (cannot have space),
        # but the product name in SPDK is "Logical Volume"
        bdev_type = bdev_type.replace("_", " ")
        for bdev in filter(lambda x: bdev_type in x["product_name"],
                           cls.current_bdevs):
            test = Bdev(bdev)
            yield test

    @classmethod
    def delete_bdev(cls, name):
        args = argparse.Namespace(client=cls.client, bdev_name=name)
        rpc_bdev.delete_bdev(args)

    @classmethod
    def create_lvol_store(cls, **kwargs):
        args = argparse.Namespace(client=cls.client, **kwargs)
        # TODO: Need to catch failed RPC calls, modify rpc.client to do that!
        response = rpc_lvol.construct_lvol_store(args)
        args = argparse.Namespace(client=cls.client, uuid=response[0], lvs_name=None)
        new_lvs = rpc_lvol.get_lvol_stores(args)
        cls.current_lvol_stores.append(new_lvs[0])
        return new_lvs[0]["name"]

    @classmethod
    def delete_lvol_store(cls, **kwargs):
        args = argparse.Namespace(client=cls.client, **kwargs)
        # TODO: Need to catch failed RPC calls, modify rpc.client to do that!
        rpc_lvol.destroy_lvol_store(args)


class Bdev():
    def __init__(self, bdev_info):
        """
        All class attributes are set based on what information is received
        from get_bdevs RPC call.
        # TODO: Document in docstring parameters which describe bdevs.
        # TODO: Possible improvement: JSON schema might be used here in future
        """
        for i in bdev_info.keys():
            setattr(self, i, bdev_info[i])


class LvolStore():
    def __init__(self, lvs_info):
        """
        All class attributes are set based on what information is received
        from get_bdevs RPC call.
        # TODO: Document in docstring parameters which describe bdevs.
        # TODO: Possible improvement: JSON schema might be used here in future
        """
        for i in lvs_info.keys():
            setattr(self, i, lvs_info[i])
