from .ui_node import UINode, UIBdevs, UILvolStores
import rpc.client
import rpc
from argparse import Namespace as an


class UIRoot(UINode):
    """
    Root node for CLI menu tree structure. Refreshes running config on startup.
    """
    def __init__(self, s, p, shell, as_root=False):
        UINode.__init__(self, '/', shell=shell)
        self.as_root = as_root
        self.current_bdevs = []
        self.current_lvol_stores = []
        self.set_rpc_target(s, p)

    def refresh(self):
        self._children = set([])
        UIBdevs(self)
        UILvolStores(self)

    def set_rpc_target(self, s, p):
        self.client = rpc.client.JSONRPCClient(s, p)

    def list_bdevs(self):
        self.current_bdevs = rpc.bdev.get_bdevs(self.client, an(name=""))

    def get_bdevs(self, bdev_type):
        # Following replace needs to be don in order for some of the bdev listings to work.
        # For example logical volumes: listing in menu is "Logical_Volume" (cannot have space),
        # but the product name in SPDK is "Logical Volume"
        bdev_type = bdev_type.replace("_", " ")
        for bdev in filter(lambda x: bdev_type in x["product_name"],
                           self.current_bdevs):
            test = Bdev(bdev)
            yield test

    def delete_bdev(self, name):
        rpc.bdev.delete_bdev(self.client, an(bdev_name=name))

    def create_malloc_bdev(self, **kwargs):
        response = rpc.bdev.construct_malloc_bdev(self.client, an(**kwargs))
        new_bdev = rpc.bdev.get_bdevs(self.client, an(name=response[0]))
        self.current_bdevs.append(new_bdev[0])
        return new_bdev[0]["name"]

    def create_aio_bdev(self, **kwargs):
        response = rpc.bdev.construct_aio_bdev(self.client, an(**kwargs))
        new_bdev = rpc.bdev.get_bdevs(self.client, an(name=response[0]))
        self.current_bdevs.append(new_bdev[0])
        return new_bdev[0]["name"]

    def create_lvol_bdev(self, **kwargs):
        response = rpc.lvol.construct_lvol_bdev(self.client, an(**kwargs))
        new_bdev = rpc.bdev.get_bdevs(self.client, an(name=response[0]))
        self.current_bdevs.append(new_bdev[0])
        return new_bdev[0]["aliases"][0], new_bdev[0]["name"]

    def list_lvols(self):
        self.current_lvol_stores = rpc.lvol.get_lvol_stores(self.client, an(lvs_name="", uuid=""))

    def get_lvol_stores(self):
        for lvs in self.current_lvol_stores:
            yield LvolStore(lvs)

    def create_lvol_store(self, **kwargs):
        response = rpc.lvol.construct_lvol_store(self.client, an(**kwargs))
        new_lvs = rpc.lvol.get_lvol_stores(self.client, an(uuid=response[0], lvs_name=None))
        self.current_lvol_stores.append(new_lvs[0])
        return new_lvs[0]["name"]

    def delete_lvol_store(self, **kwargs):
        rpc.lvol.destroy_lvol_store(self.client, an(**kwargs))


class Bdev(object):
    def __init__(self, bdev_info):
        """
        All class attributes are set based on what information is received
        from get_bdevs RPC call.
        # TODO: Document in docstring parameters which describe bdevs.
        # TODO: Possible improvement: JSON schema might be used here in future
        """
        for i in bdev_info.keys():
            setattr(self, i, bdev_info[i])


class LvolStore(object):
    def __init__(self, lvs_info):
        """
        All class attributes are set based on what information is received
        from get_bdevs RPC call.
        # TODO: Document in docstring parameters which describe bdevs.
        # TODO: Possible improvement: JSON schema might be used here in future
        """
        for i in lvs_info.keys():
            setattr(self, i, lvs_info[i])
