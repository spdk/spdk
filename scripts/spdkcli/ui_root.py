from .ui_node import UINode, UIBdevs, UILvolStores
import rpc.client
import rpc


class UIRoot(UINode):
    """
    Root node for CLI menu tree structure. Refreshes running config on startup.
    """
    def __init__(self, s, shell):
        UINode.__init__(self, "/", shell=shell)
        self.current_bdevs = []
        self.current_lvol_stores = []
        self.set_rpc_target(s)

    def refresh(self):
        self._children = set([])
        UIBdevs(self)
        UILvolStores(self)

    def set_rpc_target(self, s):
        self.client = rpc.client.JSONRPCClient(s)

    def print_array(self, a):
        return " ".join(a)

    def get_bdevs(self, bdev_type):
        self.current_bdevs = rpc.bdev.get_bdevs(self.client)
        # Following replace needs to be done in order for some of the bdev
        # listings to work: logical volumes, split disk.
        # For example logical volumes: listing in menu is "Logical_Volume"
        # (cannot have space), but the product name in SPDK is "Logical Volume"
        bdev_type = bdev_type.replace("_", " ")
        for bdev in filter(lambda x: bdev_type in x["product_name"],
                           self.current_bdevs):
            test = Bdev(bdev)
            yield test

    def get_bdevs_iostat(self, **kwargs):
        return rpc.bdev.get_bdevs_iostat(self.client, **kwargs)

    def split_bdev(self, **kwargs):
        response = rpc.bdev.construct_split_vbdev(self.client, **kwargs)
        return self.print_array(response)

    def destruct_split_bdev(self, **kwargs):
        rpc.bdev.destruct_split_vbdev(self.client, **kwargs)

    def delete_bdev(self, name):
        rpc.bdev.delete_bdev(self.client, bdev_name=name)

    def create_malloc_bdev(self, **kwargs):
        response = rpc.bdev.construct_malloc_bdev(self.client, **kwargs)
        return self.print_array(response)

    def delete_malloc_bdev(self, **kwargs):
        rpc.bdev.delete_malloc_bdev(self.client, **kwargs)

    def create_aio_bdev(self, **kwargs):
        response = rpc.bdev.construct_aio_bdev(self.client, **kwargs)
        return self.print_array(response)

    def delete_aio_bdev(self, **kwargs):
        rpc.bdev.delete_aio_bdev(self.client, **kwargs)

    def create_lvol_bdev(self, **kwargs):
        response = rpc.lvol.construct_lvol_bdev(self.client, **kwargs)
        return self.print_array(response)

    def create_nvme_bdev(self, **kwargs):
        response = rpc.bdev.construct_nvme_bdev(self.client, **kwargs)
        return self.print_array(response)

    def create_null_bdev(self, **kwargs):
        response = rpc.bdev.construct_null_bdev(self.client, **kwargs)
        return self.print_array(response)

    def create_error_bdev(self, **kwargs):
        response = rpc.bdev.construct_error_bdev(self.client, **kwargs)

    def delete_error_bdev(self, **kwargs):
        rpc.bdev.delete_error_bdev(self.client, **kwargs)

    def get_lvol_stores(self):
        self.current_lvol_stores = rpc.lvol.get_lvol_stores(self.client)
        for lvs in self.current_lvol_stores:
            yield LvolStore(lvs)

    def create_lvol_store(self, **kwargs):
        response = rpc.lvol.construct_lvol_store(self.client, **kwargs)
        new_lvs = rpc.lvol.get_lvol_stores(self.client,
                                           self.print_array(response),
                                           lvs_name=None)
        return new_lvs[0]["name"]

    def delete_lvol_store(self, **kwargs):
        rpc.lvol.destroy_lvol_store(self.client, **kwargs)

    def create_pmem_pool(self, **kwargs):
        response = rpc.pmem.create_pmem_pool(self.client, **kwargs)
        return response

    def delete_pmem_pool(self, **kwargs):
        rpc.pmem.delete_pmem_pool(self.client, **kwargs)

    def create_pmem_bdev(self, **kwargs):
        response = rpc.bdev.construct_pmem_bdev(self.client, **kwargs)
        return response

    def create_rbd_bdev(self, **kwargs):
        response = rpc.bdev.construct_rbd_bdev(self.client, **kwargs)
        return response


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
