from .ui_node import UINode, UIBdevs, UILvolStores, UIVhosts
from .ui_node_nvmf import UINVMf
from .ui_node_iscsi import UIISCSI
import rpc.client
import rpc
from functools import wraps


class UIRoot(UINode):
    """
    Root node for CLI menu tree structure. Refreshes running config on startup.
    """
    def __init__(self, s, shell):
        UINode.__init__(self, "/", shell=shell)
        self.current_bdevs = []
        self.current_lvol_stores = []
        self.current_vhost_ctrls = []
        self.current_nvmf_subsystems = []
        self.set_rpc_target(s)
        self.verbose = False
        self.is_init = self.check_init()

    def refresh(self):
        if self.is_init is False:
            methods = self.get_rpc_methods(current=True)
            methods = "\n".join(methods)
            self.shell.log.warning("SPDK Application is not yet initialized.\n"
                                   "Please initialize subsystems with start_subsystem_init command.\n"
                                   "List of available commands in current state:\n"
                                   "%s" % methods)
        else:
            # Pass because we'd like to build main tree structure for "ls"
            # even if state is uninitialized
            pass

        self._children = set([])
        UIBdevs(self)
        UILvolStores(self)
        UIVhosts(self)
        UINVMf(self)
        UIISCSI(self)

    def set_rpc_target(self, s):
        self.client = rpc.client.JSONRPCClient(s)

    def print_array(self, a):
        return " ".join(a)

    def verbose(f):
        # For any configuration calls (create, delete, construct, etc.)
        # Check if verbose option is to be used and set appropriately.
        # Do not use for "get_*" methods so that output is not
        # flooded.
        def w(self, **kwargs):
            self.client.verbose = self.verbose
            r = f(self, **kwargs)
            self.client.verbose = False
            return r
        return w

    def ui_command_start_subsystem_init(self):
        if rpc.start_subsystem_init(self.client):
            self.is_init = True
            self.refresh()

    def ui_command_load_config(self, filename):
        with open(filename, "r") as fd:
            rpc.load_config(self.client, fd)

    def ui_command_load_subsystem_config(self, filename):
        with open(filename, "r") as fd:
            rpc.load_subsystem_config(self.client, fd)

    def ui_command_save_config(self, filename, indent=2):
        with open(filename, "w") as fd:
            rpc.save_config(self.client, fd, indent)

    def ui_command_save_subsystem_config(self, filename, subsystem, indent=2):
        with open(filename, "w") as fd:
            rpc.save_subsystem_config(self.client, fd, indent, subsystem)

    def get_rpc_methods(self, current=False):
        return rpc.get_rpc_methods(self.client, current=current)

    def check_init(self):
        return "start_subsystem_init" not in self.get_rpc_methods(current=True)

    def get_bdevs(self, bdev_type):
        if self.is_init:
            self.current_bdevs = rpc.bdev.get_bdevs(self.client)
            # Following replace needs to be done in order for some of the bdev
            # listings to work: logical volumes, split disk.
            # For example logical volumes: listing in menu is "Logical_Volume"
            # (cannot have space), but the product name in SPDK is "Logical Volume"
            bdev_type = bdev_type.replace("_", " ")
            for bdev in [x for x in self.current_bdevs if bdev_type in x["product_name"].lower()]:
                test = Bdev(bdev)
                yield test

    def get_bdevs_iostat(self, **kwargs):
        return rpc.bdev.get_bdevs_iostat(self.client, **kwargs)

    @verbose
    def split_bdev(self, **kwargs):
        response = rpc.bdev.construct_split_vbdev(self.client, **kwargs)
        return self.print_array(response)

    @verbose
    def destruct_split_bdev(self, **kwargs):
        rpc.bdev.destruct_split_vbdev(self.client, **kwargs)

    @verbose
    def delete_bdev(self, name):
        rpc.bdev.delete_bdev(self.client, bdev_name=name)

    @verbose
    def create_malloc_bdev(self, **kwargs):
        response = rpc.bdev.construct_malloc_bdev(self.client, **kwargs)
        return response

    @verbose
    def delete_malloc_bdev(self, **kwargs):
        rpc.bdev.delete_malloc_bdev(self.client, **kwargs)

    @verbose
    def create_iscsi_bdev(self, **kwargs):
        response = rpc.bdev.construct_iscsi_bdev(self.client, **kwargs)
        return response

    @verbose
    def delete_iscsi_bdev(self, **kwargs):
        rpc.bdev.delete_iscsi_bdev(self.client, **kwargs)

    @verbose
    def create_aio_bdev(self, **kwargs):
        response = rpc.bdev.construct_aio_bdev(self.client, **kwargs)
        return response

    @verbose
    def delete_aio_bdev(self, **kwargs):
        rpc.bdev.delete_aio_bdev(self.client, **kwargs)

    @verbose
    def create_lvol_bdev(self, **kwargs):
        response = rpc.lvol.construct_lvol_bdev(self.client, **kwargs)
        return response

    @verbose
    def destroy_lvol_bdev(self, **kwargs):
        response = rpc.lvol.destroy_lvol_bdev(self.client, **kwargs)
        return response

    @verbose
    def create_nvme_bdev(self, **kwargs):
        response = rpc.bdev.construct_nvme_bdev(self.client, **kwargs)
        return response

    @verbose
    def delete_nvme_controller(self, **kwargs):
        rpc.bdev.delete_nvme_controller(self.client, **kwargs)

    @verbose
    def create_null_bdev(self, **kwargs):
        response = rpc.bdev.construct_null_bdev(self.client, **kwargs)
        return response

    @verbose
    def delete_null_bdev(self, **kwargs):
        rpc.bdev.delete_null_bdev(self.client, **kwargs)

    @verbose
    def create_error_bdev(self, **kwargs):
        response = rpc.bdev.construct_error_bdev(self.client, **kwargs)

    @verbose
    def delete_error_bdev(self, **kwargs):
        rpc.bdev.delete_error_bdev(self.client, **kwargs)

    def get_lvol_stores(self):
        if self.is_init:
            self.current_lvol_stores = rpc.lvol.get_lvol_stores(self.client)
            for lvs in self.current_lvol_stores:
                yield LvolStore(lvs)

    @verbose
    def create_lvol_store(self, **kwargs):
        response = rpc.lvol.construct_lvol_store(self.client, **kwargs)
        return response

    @verbose
    def delete_lvol_store(self, **kwargs):
        rpc.lvol.destroy_lvol_store(self.client, **kwargs)

    @verbose
    def create_pmem_pool(self, **kwargs):
        response = rpc.pmem.create_pmem_pool(self.client, **kwargs)
        return response

    @verbose
    def delete_pmem_pool(self, **kwargs):
        rpc.pmem.delete_pmem_pool(self.client, **kwargs)

    @verbose
    def create_pmem_bdev(self, **kwargs):
        response = rpc.bdev.construct_pmem_bdev(self.client, **kwargs)
        return response

    @verbose
    def delete_pmem_bdev(self, **kwargs):
        response = rpc.bdev.delete_pmem_bdev(self.client, **kwargs)
        return response

    @verbose
    def create_rbd_bdev(self, **kwargs):
        response = rpc.bdev.construct_rbd_bdev(self.client, **kwargs)
        return response

    @verbose
    def delete_rbd_bdev(self, **kwargs):
        response = rpc.bdev.delete_rbd_bdev(self.client, **kwargs)
        return response

    @verbose
    def create_virtio_dev(self, **kwargs):
        response = rpc.vhost.construct_virtio_dev(self.client, **kwargs)
        return self.print_array(response)

    @verbose
    def remove_virtio_bdev(self, **kwargs):
        response = rpc.vhost.remove_virtio_bdev(self.client, **kwargs)
        return response

    def get_virtio_scsi_devs(self):
        if self.is_init:
            for bdev in rpc.vhost.get_virtio_scsi_devs(self.client):
                test = Bdev(bdev)
                yield test

    def list_vhost_ctrls(self):
        if self.is_init:
            self.current_vhost_ctrls = rpc.vhost.get_vhost_controllers(self.client)

    def get_vhost_ctrlrs(self, ctrlr_type):
        if self.is_init:
            self.list_vhost_ctrls()
            for ctrlr in [x for x in self.current_vhost_ctrls if ctrlr_type in list(x["backend_specific"].keys())]:
                yield VhostCtrlr(ctrlr)

    @verbose
    def remove_vhost_controller(self, **kwargs):
        rpc.vhost.remove_vhost_controller(self.client, **kwargs)

    @verbose
    def create_vhost_scsi_controller(self, **kwargs):
        rpc.vhost.construct_vhost_scsi_controller(self.client, **kwargs)

    @verbose
    def create_vhost_blk_controller(self, **kwargs):
        rpc.vhost.construct_vhost_blk_controller(self.client, **kwargs)

    @verbose
    def remove_vhost_scsi_target(self, **kwargs):
        rpc.vhost.remove_vhost_scsi_target(self.client, **kwargs)

    @verbose
    def add_vhost_scsi_lun(self, **kwargs):
        rpc.vhost.add_vhost_scsi_lun(self.client, **kwargs)

    def set_vhost_controller_coalescing(self, **kwargs):
        rpc.vhost.set_vhost_controller_coalescing(self.client, **kwargs)

    def list_nvmf_subsystems(self):
        if self.is_init:
            self.current_nvmf_subsystems = rpc.nvmf.get_nvmf_subsystems(self.client)

    def get_nvmf_subsystems(self):
        if self.is_init:
            self.list_nvmf_subsystems()
            for subsystem in self.current_nvmf_subsystems:
                yield NvmfSubsystem(subsystem)

    @verbose
    def create_nvmf_subsystem(self, **kwargs):
        rpc.nvmf.nvmf_subsystem_create(self.client, **kwargs)

    @verbose
    def delete_nvmf_subsystem(self, **kwargs):
        rpc.nvmf.delete_nvmf_subsystem(self.client, **kwargs)

    @verbose
    def nvmf_subsystem_add_listener(self, **kwargs):
        rpc.nvmf.nvmf_subsystem_add_listener(self.client, **kwargs)

    @verbose
    def nvmf_subsystem_remove_listener(self, **kwargs):
        rpc.nvmf.nvmf_subsystem_remove_listener(self.client, **kwargs)

    @verbose
    def nvmf_subsystem_add_host(self, **kwargs):
        rpc.nvmf.nvmf_subsystem_add_host(self.client, **kwargs)

    @verbose
    def nvmf_subsystem_remove_host(self, **kwargs):
        rpc.nvmf.nvmf_subsystem_remove_host(self.client, **kwargs)

    @verbose
    def nvmf_subsystem_allow_any_host(self, **kwargs):
        rpc.nvmf.nvmf_subsystem_allow_any_host(self.client, **kwargs)

    @verbose
    def nvmf_subsystem_add_ns(self, **kwargs):
        rpc.nvmf.nvmf_subsystem_add_ns(self.client, **kwargs)

    @verbose
    def nvmf_subsystem_remove_ns(self, **kwargs):
        rpc.nvmf.nvmf_subsystem_remove_ns(self.client, **kwargs)

    @verbose
    def nvmf_subsystem_allow_any_host(self, **kwargs):
        rpc.nvmf.nvmf_subsystem_allow_any_host(self.client, **kwargs)

    def get_scsi_devices(self):
        if self.is_init:
            for device in rpc.iscsi.get_scsi_devices(self.client):
                yield ScsiObj(device)

    def get_target_nodes(self):
        if self.is_init:
            for tg in rpc.iscsi.get_target_nodes(self.client):
                yield tg

    @verbose
    def construct_target_node(self, **kwargs):
        rpc.iscsi.construct_target_node(self.client, **kwargs)

    @verbose
    def delete_target_node(self, **kwargs):
        rpc.iscsi.delete_target_node(self.client, **kwargs)

    def get_portal_groups(self):
        if self.is_init:
            for pg in rpc.iscsi.get_portal_groups(self.client):
                yield ScsiObj(pg)

    def get_initiator_groups(self):
        if self.is_init:
            for ig in rpc.iscsi.get_initiator_groups(self.client):
                yield ScsiObj(ig)

    @verbose
    def construct_portal_group(self, **kwargs):
        rpc.iscsi.add_portal_group(self.client, **kwargs)

    @verbose
    def delete_portal_group(self, **kwargs):
        rpc.iscsi.delete_portal_group(self.client, **kwargs)

    @verbose
    def construct_initiator_group(self, **kwargs):
        rpc.iscsi.add_initiator_group(self.client, **kwargs)

    @verbose
    def delete_initiator_group(self, **kwargs):
        rpc.iscsi.delete_initiator_group(self.client, **kwargs)

    @verbose
    def get_iscsi_connections(self, **kwargs):
        if self.is_init:
            for ic in rpc.iscsi.get_iscsi_connections(self.client, **kwargs):
                yield ic

    @verbose
    def add_initiators_to_initiator_group(self, **kwargs):
        rpc.iscsi.add_initiators_to_initiator_group(self.client, **kwargs)

    @verbose
    def delete_initiators_from_initiator_group(self, **kwargs):
        rpc.iscsi.delete_initiators_from_initiator_group(self.client, **kwargs)

    @verbose
    def add_pg_ig_maps(self, **kwargs):
        rpc.iscsi.add_pg_ig_maps(self.client, **kwargs)

    @verbose
    def delete_pg_ig_maps(self, **kwargs):
        rpc.iscsi.delete_pg_ig_maps(self.client, **kwargs)

    @verbose
    def add_secret_to_iscsi_auth_group(self, **kwargs):
        rpc.iscsi.add_secret_to_iscsi_auth_group(self.client, **kwargs)

    @verbose
    def delete_secret_from_iscsi_auth_group(self, **kwargs):
        rpc.iscsi.delete_secret_from_iscsi_auth_group(self.client, **kwargs)

    @verbose
    def get_iscsi_auth_groups(self, **kwargs):
        return rpc.iscsi.get_iscsi_auth_groups(self.client, **kwargs)

    @verbose
    def add_iscsi_auth_group(self, **kwargs):
        rpc.iscsi.add_iscsi_auth_group(self.client, **kwargs)

    @verbose
    def delete_iscsi_auth_group(self, **kwargs):
        rpc.iscsi.delete_iscsi_auth_group(self.client, **kwargs)

    @verbose
    def set_iscsi_target_node_auth(self, **kwargs):
        rpc.iscsi.set_iscsi_target_node_auth(self.client, **kwargs)

    @verbose
    def target_node_add_lun(self, **kwargs):
        rpc.iscsi.target_node_add_lun(self.client, **kwargs)

    @verbose
    def set_iscsi_discovery_auth(self, **kwargs):
        rpc.iscsi.set_iscsi_discovery_auth(self.client, **kwargs)

    @verbose
    def get_iscsi_global_params(self, **kwargs):
        return rpc.iscsi.get_iscsi_global_params(self.client, **kwargs)


class Bdev(object):
    def __init__(self, bdev_info):
        """
        All class attributes are set based on what information is received
        from get_bdevs RPC call.
        # TODO: Document in docstring parameters which describe bdevs.
        # TODO: Possible improvement: JSON schema might be used here in future
        """
        for i in list(bdev_info.keys()):
            setattr(self, i, bdev_info[i])


class LvolStore(object):
    def __init__(self, lvs_info):
        """
        All class attributes are set based on what information is received
        from get_bdevs RPC call.
        # TODO: Document in docstring parameters which describe bdevs.
        # TODO: Possible improvement: JSON schema might be used here in future
        """
        for i in list(lvs_info.keys()):
            setattr(self, i, lvs_info[i])


class VhostCtrlr(object):
    def __init__(self, ctrlr_info):
        """
        All class attributes are set based on what information is received
        from get_vhost_controllers RPC call.
        # TODO: Document in docstring parameters which describe bdevs.
        # TODO: Possible improvement: JSON schema might be used here in future
        """
        for i in list(ctrlr_info.keys()):
            setattr(self, i, ctrlr_info[i])


class NvmfSubsystem(object):
    def __init__(self, subsystem_info):
        """
        All class attributes are set based on what information is received
        from get_nvmf_subsystem RPC call.
        # TODO: Document in docstring parameters which describe bdevs.
        # TODO: Possible improvement: JSON schema might be used here in future
        """
        for i in subsystem_info.keys():
            setattr(self, i, subsystem_info[i])


class ScsiObj(object):
    def __init__(self, device_info):
        """
        All class attributes are set based on what information is received
        from iscsi related RPC calls.
        # TODO: Document in docstring parameters which describe bdevs.
        # TODO: Possible improvement: JSON schema might be used here in future
        """
        for i in device_info.keys():
            setattr(self, i, device_info[i])
