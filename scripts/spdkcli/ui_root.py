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
    def __init__(self, client, shell):
        UINode.__init__(self, "/", shell=shell)
        self.current_bdevs = []
        self.current_lvol_stores = []
        self.current_vhost_ctrls = []
        self.current_nvmf_transports = []
        self.current_nvmf_subsystems = []
        self.set_rpc_target(client)
        self.verbose = False
        self.is_init = self.check_init()
        self.methods = []

    def refresh(self):
        self.methods = self.rpc_get_methods(current=True)
        if self.is_init is False:
            methods = "\n".join(self.methods)
            self.shell.log.warning("SPDK Application is not yet initialized.\n"
                                   "Please initialize subsystems with framework_start_init command.\n"
                                   "List of available commands in current state:\n"
                                   "%s" % methods)
        else:
            # Pass because we'd like to build main tree structure for "ls"
            # even if state is uninitialized
            pass

        self._children = set([])
        UIBdevs(self)
        UILvolStores(self)
        if self.has_subsystem("vhost"):
            UIVhosts(self)
        if self.has_subsystem("nvmf"):
            UINVMf(self)
        if self.has_subsystem("iscsi"):
            UIISCSI(self)

    def set_rpc_target(self, client):
        self.client = client

    def print_array(self, a):
        return " ".join(a)

    def verbose(f):
        # For any configuration calls (create, delete, construct, etc.)
        # Check if verbose option is to be used and set appropriately.
        # Do not use for "get_*" methods so that output is not
        # flooded.
        def w(self, **kwargs):
            self.client.log_set_level("INFO" if self.verbose else "ERROR")
            r = f(self, **kwargs)
            self.client.log_set_level("ERROR")
            return r
        return w

    def is_method_available(f):
        # Check if method f is available for given spdk target
        def w(self, **kwargs):
            if f.__name__ in self.methods:
                r = f(self, **kwargs)
                return r
            # If given method is not available return empty list
            # similar to real get_* like rpc
            return []
        return w

    def ui_command_framework_start_init(self):
        if rpc.framework_start_init(self.client):
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

    def rpc_get_methods(self, current=False):
        return rpc.rpc_get_methods(self.client, current=current)

    def check_init(self):
        return "framework_start_init" not in self.rpc_get_methods(current=True)

    def bdev_get_bdevs(self, bdev_type):
        if self.is_init:
            self.current_bdevs = rpc.bdev.bdev_get_bdevs(self.client)
            # Following replace needs to be done in order for some of the bdev
            # listings to work: logical volumes, split disk.
            # For example logical volumes: listing in menu is "Logical_Volume"
            # (cannot have space), but the product name in SPDK is "Logical Volume"
            bdev_type = bdev_type.replace("_", " ")
            for bdev in [x for x in self.current_bdevs if bdev_type in x["product_name"].lower()]:
                test = Bdev(bdev)
                yield test

    def bdev_get_iostat(self, **kwargs):
        return rpc.bdev.bdev_get_iostat(self.client, **kwargs)

    @verbose
    def bdev_split_create(self, **kwargs):
        response = rpc.bdev.bdev_split_create(self.client, **kwargs)
        return self.print_array(response)

    @verbose
    def bdev_split_delete(self, **kwargs):
        rpc.bdev.bdev_split_delete(self.client, **kwargs)

    @verbose
    def create_malloc_bdev(self, **kwargs):
        response = rpc.bdev.bdev_malloc_create(self.client, **kwargs)
        return response

    @verbose
    def bdev_malloc_delete(self, **kwargs):
        rpc.bdev.bdev_malloc_delete(self.client, **kwargs)

    @verbose
    def create_iscsi_bdev(self, **kwargs):
        response = rpc.bdev.bdev_iscsi_create(self.client, **kwargs)
        return response

    @verbose
    def bdev_iscsi_delete(self, **kwargs):
        rpc.bdev.bdev_iscsi_delete(self.client, **kwargs)

    @verbose
    def bdev_aio_create(self, **kwargs):
        response = rpc.bdev.bdev_aio_create(self.client, **kwargs)
        return response

    @verbose
    def bdev_aio_delete(self, **kwargs):
        rpc.bdev.bdev_aio_delete(self.client, **kwargs)

    @verbose
    def create_lvol_bdev(self, **kwargs):
        response = rpc.lvol.bdev_lvol_create(self.client, **kwargs)
        return response

    @verbose
    def bdev_lvol_delete(self, **kwargs):
        response = rpc.lvol.bdev_lvol_delete(self.client, **kwargs)
        return response

    @verbose
    def create_nvme_bdev(self, **kwargs):
        response = rpc.bdev.bdev_nvme_attach_controller(self.client, **kwargs)
        return response

    @verbose
    def bdev_nvme_detach_controller(self, **kwargs):
        rpc.bdev.bdev_nvme_detach_controller(self.client, **kwargs)

    @verbose
    def bdev_null_create(self, **kwargs):
        response = rpc.bdev.bdev_null_create(self.client, **kwargs)
        return response

    @verbose
    def bdev_null_delete(self, **kwargs):
        rpc.bdev.bdev_null_delete(self.client, **kwargs)

    @verbose
    def create_error_bdev(self, **kwargs):
        response = rpc.bdev.bdev_error_create(self.client, **kwargs)

    @verbose
    def bdev_error_delete(self, **kwargs):
        rpc.bdev.bdev_error_delete(self.client, **kwargs)

    @verbose
    @is_method_available
    def bdev_lvol_get_lvstores(self):
        if self.is_init:
            self.current_lvol_stores = rpc.lvol.bdev_lvol_get_lvstores(self.client)
            for lvs in self.current_lvol_stores:
                yield LvolStore(lvs)

    @verbose
    def bdev_lvol_create_lvstore(self, **kwargs):
        response = rpc.lvol.bdev_lvol_create_lvstore(self.client, **kwargs)
        return response

    @verbose
    def bdev_lvol_delete_lvstore(self, **kwargs):
        rpc.lvol.bdev_lvol_delete_lvstore(self.client, **kwargs)

    @verbose
    def bdev_pmem_create_pool(self, **kwargs):
        response = rpc.pmem.bdev_pmem_create_pool(self.client, **kwargs)
        return response

    @verbose
    def bdev_pmem_delete_pool(self, **kwargs):
        rpc.pmem.bdev_pmem_delete_pool(self.client, **kwargs)

    @verbose
    def bdev_pmem_get_pool_info(self, **kwargs):
        response = rpc.pmem.bdev_pmem_get_pool_info(self.client, **kwargs)
        return response

    @verbose
    def bdev_pmem_create(self, **kwargs):
        response = rpc.bdev.bdev_pmem_create(self.client, **kwargs)
        return response

    @verbose
    def bdev_pmem_delete(self, **kwargs):
        response = rpc.bdev.bdev_pmem_delete(self.client, **kwargs)
        return response

    @verbose
    def create_rbd_bdev(self, **kwargs):
        response = rpc.bdev.bdev_rbd_create(self.client, **kwargs)
        return response

    @verbose
    def bdev_rbd_delete(self, **kwargs):
        response = rpc.bdev.bdev_rbd_delete(self.client, **kwargs)
        return response

    @verbose
    def create_virtio_dev(self, **kwargs):
        response = rpc.vhost.bdev_virtio_attach_controller(self.client, **kwargs)
        return self.print_array(response)

    @verbose
    def bdev_virtio_detach_controller(self, **kwargs):
        response = rpc.vhost.bdev_virtio_detach_controller(self.client, **kwargs)
        return response

    @verbose
    def bdev_raid_create(self, **kwargs):
        rpc.bdev.bdev_raid_create(self.client, **kwargs)

    @verbose
    def bdev_raid_delete(self, **kwargs):
        rpc.bdev.bdev_raid_delete(self.client, **kwargs)

    @verbose
    @is_method_available
    def bdev_virtio_scsi_get_devices(self):
        if self.is_init:
            for bdev in rpc.vhost.bdev_virtio_scsi_get_devices(self.client):
                test = Bdev(bdev)
                yield test

    def list_vhost_ctrls(self):
        if self.is_init:
            self.current_vhost_ctrls = rpc.vhost.vhost_get_controllers(self.client)

    @verbose
    @is_method_available
    def vhost_get_controllers(self, ctrlr_type):
        if self.is_init:
            self.list_vhost_ctrls()
            for ctrlr in [x for x in self.current_vhost_ctrls if ctrlr_type in list(x["backend_specific"].keys())]:
                yield VhostCtrlr(ctrlr)

    @verbose
    def vhost_delete_controller(self, **kwargs):
        rpc.vhost.vhost_delete_controller(self.client, **kwargs)

    @verbose
    def vhost_create_scsi_controller(self, **kwargs):
        rpc.vhost.vhost_create_scsi_controller(self.client, **kwargs)

    @verbose
    def vhost_create_blk_controller(self, **kwargs):
        rpc.vhost.vhost_create_blk_controller(self.client, **kwargs)

    @verbose
    def vhost_scsi_controller_remove_target(self, **kwargs):
        rpc.vhost.vhost_scsi_controller_remove_target(self.client, **kwargs)

    @verbose
    def vhost_scsi_controller_add_target(self, **kwargs):
        rpc.vhost.vhost_scsi_controller_add_target(self.client, **kwargs)

    def vhost_controller_set_coalescing(self, **kwargs):
        rpc.vhost.vhost_controller_set_coalescing(self.client, **kwargs)

    @verbose
    def create_nvmf_transport(self, **kwargs):
        rpc.nvmf.nvmf_create_transport(self.client, **kwargs)

    def list_nvmf_transports(self):
        if self.is_init:
            self.current_nvmf_transports = rpc.nvmf.nvmf_get_transports(self.client)

    @verbose
    @is_method_available
    def nvmf_get_transports(self):
        if self.is_init:
            self.list_nvmf_transports()
            for transport in self.current_nvmf_transports:
                yield NvmfTransport(transport)

    def list_nvmf_subsystems(self):
        if self.is_init:
            self.current_nvmf_subsystems = rpc.nvmf.nvmf_get_subsystems(self.client)

    @verbose
    @is_method_available
    def nvmf_get_subsystems(self):
        if self.is_init:
            self.list_nvmf_subsystems()
            for subsystem in self.current_nvmf_subsystems:
                yield NvmfSubsystem(subsystem)

    @verbose
    def create_nvmf_subsystem(self, **kwargs):
        rpc.nvmf.nvmf_create_subsystem(self.client, **kwargs)

    @verbose
    def nvmf_delete_subsystem(self, **kwargs):
        rpc.nvmf.nvmf_delete_subsystem(self.client, **kwargs)

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

    @verbose
    @is_method_available
    def scsi_get_devices(self):
        if self.is_init:
            for device in rpc.iscsi.scsi_get_devices(self.client):
                yield ScsiObj(device)

    @verbose
    @is_method_available
    def iscsi_get_target_nodes(self):
        if self.is_init:
            for tg in rpc.iscsi.iscsi_get_target_nodes(self.client):
                yield tg

    @verbose
    def iscsi_create_target_node(self, **kwargs):
        rpc.iscsi.iscsi_create_target_node(self.client, **kwargs)

    @verbose
    def iscsi_delete_target_node(self, **kwargs):
        rpc.iscsi.iscsi_delete_target_node(self.client, **kwargs)

    @verbose
    @is_method_available
    def iscsi_get_portal_groups(self):
        if self.is_init:
            for pg in rpc.iscsi.iscsi_get_portal_groups(self.client):
                yield ScsiObj(pg)

    @verbose
    @is_method_available
    def iscsi_get_initiator_groups(self):
        if self.is_init:
            for ig in rpc.iscsi.iscsi_get_initiator_groups(self.client):
                yield ScsiObj(ig)

    @verbose
    def construct_portal_group(self, **kwargs):
        rpc.iscsi.iscsi_create_portal_group(self.client, **kwargs)

    @verbose
    def iscsi_delete_portal_group(self, **kwargs):
        rpc.iscsi.iscsi_delete_portal_group(self.client, **kwargs)

    @verbose
    def construct_initiator_group(self, **kwargs):
        rpc.iscsi.iscsi_create_initiator_group(self.client, **kwargs)

    @verbose
    def iscsi_delete_initiator_group(self, **kwargs):
        rpc.iscsi.iscsi_delete_initiator_group(self.client, **kwargs)

    @verbose
    @is_method_available
    def iscsi_get_connections(self, **kwargs):
        if self.is_init:
            for ic in rpc.iscsi.iscsi_get_connections(self.client, **kwargs):
                yield ic

    @verbose
    def iscsi_initiator_group_add_initiators(self, **kwargs):
        rpc.iscsi.iscsi_initiator_group_add_initiators(self.client, **kwargs)

    @verbose
    def iscsi_initiator_group_remove_initiators(self, **kwargs):
        rpc.iscsi.iscsi_initiator_group_remove_initiators(self.client, **kwargs)

    @verbose
    def iscsi_target_node_add_pg_ig_maps(self, **kwargs):
        rpc.iscsi.iscsi_target_node_add_pg_ig_maps(self.client, **kwargs)

    @verbose
    def iscsi_target_node_remove_pg_ig_maps(self, **kwargs):
        rpc.iscsi.iscsi_target_node_remove_pg_ig_maps(self.client, **kwargs)

    @verbose
    def iscsi_auth_group_add_secret(self, **kwargs):
        rpc.iscsi.iscsi_auth_group_add_secret(self.client, **kwargs)

    @verbose
    def iscsi_auth_group_remove_secret(self, **kwargs):
        rpc.iscsi.iscsi_auth_group_remove_secret(self.client, **kwargs)

    @verbose
    @is_method_available
    def iscsi_get_auth_groups(self, **kwargs):
        return rpc.iscsi.iscsi_get_auth_groups(self.client, **kwargs)

    @verbose
    def iscsi_create_auth_group(self, **kwargs):
        rpc.iscsi.iscsi_create_auth_group(self.client, **kwargs)

    @verbose
    def iscsi_delete_auth_group(self, **kwargs):
        rpc.iscsi.iscsi_delete_auth_group(self.client, **kwargs)

    @verbose
    def iscsi_target_node_set_auth(self, **kwargs):
        rpc.iscsi.iscsi_target_node_set_auth(self.client, **kwargs)

    @verbose
    def iscsi_target_node_add_lun(self, **kwargs):
        rpc.iscsi.iscsi_target_node_add_lun(self.client, **kwargs)

    @verbose
    def iscsi_set_discovery_auth(self, **kwargs):
        rpc.iscsi.iscsi_set_discovery_auth(self.client, **kwargs)

    @verbose
    @is_method_available
    def iscsi_get_options(self, **kwargs):
        return rpc.iscsi.iscsi_get_options(self.client, **kwargs)

    def has_subsystem(self, subsystem):
        for system in rpc.subsystem.framework_get_subsystems(self.client):
            if subsystem.lower() == system["subsystem"].lower():
                return True
        return False


class Bdev(object):
    def __init__(self, bdev_info):
        """
        All class attributes are set based on what information is received
        from bdev_get_bdevs RPC call.
        # TODO: Document in docstring parameters which describe bdevs.
        # TODO: Possible improvement: JSON schema might be used here in future
        """
        for i in list(bdev_info.keys()):
            setattr(self, i, bdev_info[i])


class LvolStore(object):
    def __init__(self, lvs_info):
        """
        All class attributes are set based on what information is received
        from bdev_get_bdevs RPC call.
        # TODO: Document in docstring parameters which describe bdevs.
        # TODO: Possible improvement: JSON schema might be used here in future
        """
        for i in list(lvs_info.keys()):
            setattr(self, i, lvs_info[i])


class VhostCtrlr(object):
    def __init__(self, ctrlr_info):
        """
        All class attributes are set based on what information is received
        from vhost_get_controllers RPC call.
        # TODO: Document in docstring parameters which describe bdevs.
        # TODO: Possible improvement: JSON schema might be used here in future
        """
        for i in list(ctrlr_info.keys()):
            setattr(self, i, ctrlr_info[i])


class NvmfTransport(object):
    def __init__(self, transport_info):
        """
        All class attributes are set based on what information is received
        from get_nvmf_transport RPC call.
        # TODO: Document in docstring parameters which describe bdevs.
        # TODO: Possible improvement: JSON schema might be used here in future
        """
        for i in transport_info.keys():
            setattr(self, i, transport_info[i])


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
