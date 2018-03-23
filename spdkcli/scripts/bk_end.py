import sys
import rpc.client
import rpc
from argparse import Namespace as an


class BKRoot():
    """
    TODO: rework this class in future. It mostly has classmethods and
    is not instantiated so so info about rpc client and methods could be
    probably kept somewhere else.
    """
    current_bdevs = []
    current_lvol_stores = []

    @classmethod
    def set_rpc_target(cls, s, p):
        cls.client = rpc.client.JSONRPCClient(s, p)

    @classmethod
    def list_bdevs(cls):
        BKRoot.current_bdevs = rpc.bdev.get_bdevs(cls.client, an(name=""))

    @classmethod
    def list_lvols(cls):
        BKRoot.current_lvol_stores = rpc.lvol.get_lvol_stores(cls.client,
                                                              an(lvs_name="", uuid=""))

    @classmethod
    def get_lvol_stores(cls):
        for lvs in cls.current_lvol_stores:
            yield LvolStore(lvs)

    @classmethod
    def create_malloc_bdev(cls, **kwargs):
        response = rpc.bdev.construct_malloc_bdev(cls.client, an(**kwargs))
        new_bdev = rpc.bdev.get_bdevs(cls.client, an(name=response[0]))
        cls.current_bdevs.append(new_bdev[0])
        return new_bdev[0]["name"]

    @classmethod
    def create_aio_bdev(cls, **kwargs):
        response = rpc.bdev.construct_aio_bdev(cls.client, an(**kwargs))
        new_bdev = rpc.bdev.get_bdevs(cls.client, an(name=response[0]))
        cls.current_bdevs.append(new_bdev[0])
        return new_bdev[0]["name"]

    @classmethod
    def create_lvol_bdev(cls, **kwargs):
        response = rpc.lvol.construct_lvol_bdev(cls.client, an(**kwargs))
        new_bdev = rpc.bdev.get_bdevs(cls.client, an(name=response[0]))
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
        rpc.bdev.delete_bdev(cls.client, an(bdev_name=name))

    @classmethod
    def create_lvol_store(cls, **kwargs):
        response = rpc.lvol.construct_lvol_store(cls.client, an(**kwargs))
        new_lvs = rpc.lvol.get_lvol_stores(cls.client, an(uuid=response[0], lvs_name=None))
        cls.current_lvol_stores.append(new_lvs[0])
        return new_lvs[0]["name"]

    @classmethod
    def delete_lvol_store(cls, **kwargs):
        rpc.lvol.destroy_lvol_store(cls.client, an(**kwargs))


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
