#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2018 Intel Corporation.
#  All rights reserved.

from ..rpc import config
from ..rpc.cmd_parser import apply_defaults, group_as, remove_null, strip_globals
from .ui_node import UIBdevs, UILvolStores, UINode, UIVhosts
from .ui_node_iscsi import UIISCSI
from .ui_node_nvmf import UINVMf


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
        self.current_nvmf_referrals = []
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
        if self.has_subsystem("vhost_scsi") or self.has_subsystem("vhost_blk"):
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
        if self.client.framework_start_init():
            self.is_init = True
            self.refresh()

    def ui_command_load_config(self, filename):
        with open(filename, "r") as fd:
            config.load_config(self.client, fd)

    def ui_command_load_subsystem_config(self, filename):
        with open(filename, "r") as fd:
            config.load_subsystem_config(self.client, fd)

    def ui_command_save_config(self, filename, indent=2):
        with open(filename, "w") as fd:
            config.save_config(self.client, fd, indent)

    def ui_command_save_subsystem_config(self, filename, subsystem, indent=2):
        with open(filename, "w") as fd:
            config.save_subsystem_config(self.client, fd, indent, subsystem)

    def rpc_get_methods(self, current=False):
        return self.client.rpc_get_methods(current=current)

    def check_init(self):
        return "framework_start_init" not in self.rpc_get_methods(current=True)

    def bdev_get_bdevs(self, bdev_type):
        if self.is_init:
            self.current_bdevs = self.client.bdev_get_bdevs()
            # Following replace needs to be done in order for some of the bdev
            # listings to work: logical volumes, split disk.
            # For example logical volumes: listing in menu is "Logical_Volume"
            # (cannot have space), but the product name in SPDK is "Logical Volume"
            bdev_type = bdev_type.replace("_", " ")
            for bdev in [x for x in self.current_bdevs if bdev_type in x["product_name"].lower()]:
                test = Bdev(bdev)
                yield test

    def bdev_get_iostat(self, **kwargs):
        return self.client.bdev_get_iostat(**kwargs)

    @verbose
    def bdev_split_create(self, **kwargs):
        response = self.client.bdev_split_create(**kwargs)
        return self.print_array(response)

    @verbose
    def bdev_split_delete(self, **kwargs):
        self.client.bdev_split_delete(**kwargs)

    @verbose
    def create_malloc_bdev(self, **kwargs):
        response = self.client.bdev_malloc_create(**kwargs)
        return response

    @verbose
    def bdev_malloc_delete(self, **kwargs):
        self.client.bdev_malloc_delete(**kwargs)

    @verbose
    def create_iscsi_bdev(self, **kwargs):
        response = self.client.bdev_iscsi_create(**kwargs)
        return response

    @verbose
    def bdev_iscsi_delete(self, **kwargs):
        self.client.bdev_iscsi_delete(**kwargs)

    @verbose
    def bdev_aio_create(self, **kwargs):
        response = self.client.bdev_aio_create(**kwargs)
        return response

    @verbose
    def bdev_aio_delete(self, **kwargs):
        self.client.bdev_aio_delete(**kwargs)

    @verbose
    def create_lvol_bdev(self, **kwargs):
        response = self.client.bdev_lvol_create(**kwargs)
        return response

    @verbose
    def bdev_lvol_delete(self, **kwargs):
        response = self.client.bdev_lvol_delete(**kwargs)
        return response

    @verbose
    def create_nvme_bdev(self, **kwargs):
        response = self.client.bdev_nvme_attach_controller(**kwargs)
        return response

    @verbose
    def bdev_nvme_detach_controller(self, **kwargs):
        self.client.bdev_nvme_detach_controller(**kwargs)

    @verbose
    def bdev_null_create(self, **kwargs):
        response = self.client.bdev_null_create(**kwargs)
        return response

    @verbose
    def bdev_null_delete(self, **kwargs):
        self.client.bdev_null_delete(**kwargs)

    @verbose
    def create_error_bdev(self, **kwargs):
        response = self.client.bdev_error_create(**kwargs)
        return response

    @verbose
    def bdev_error_delete(self, **kwargs):
        self.client.bdev_error_delete(**kwargs)

    @verbose
    @is_method_available
    def bdev_lvol_get_lvstores(self):
        if self.is_init:
            self.current_lvol_stores = self.client.bdev_lvol_get_lvstores()
            for lvs in self.current_lvol_stores:
                yield LvolStore(lvs)

    @verbose
    def bdev_lvol_create_lvstore(self, **kwargs):
        response = self.client.bdev_lvol_create_lvstore(**kwargs)
        return response

    @verbose
    def bdev_lvol_delete_lvstore(self, **kwargs):
        self.client.bdev_lvol_delete_lvstore(**kwargs)

    @verbose
    def create_rbd_bdev(self, **kwargs):
        response = self.client.bdev_rbd_create(**kwargs)
        return response

    @verbose
    def bdev_rbd_delete(self, **kwargs):
        response = self.client.bdev_rbd_delete(**kwargs)
        return response

    @verbose
    def create_virtio_dev(self, **kwargs):
        response = self.client.bdev_virtio_attach_controller(**kwargs)
        return self.print_array(response)

    @verbose
    def bdev_virtio_detach_controller(self, **kwargs):
        response = self.client.bdev_virtio_detach_controller(**kwargs)
        return response

    @verbose
    def bdev_raid_create(self, **kwargs):
        self.client.bdev_raid_create(**kwargs)

    @verbose
    def bdev_raid_delete(self, **kwargs):
        self.client.bdev_raid_delete(**kwargs)

    @verbose
    def bdev_uring_create(self, **kwargs):
        response = self.client.bdev_uring_create(**kwargs)
        return response

    @verbose
    def bdev_uring_delete(self, **kwargs):
        self.client.bdev_uring_delete(**kwargs)

    @verbose
    @is_method_available
    def bdev_virtio_scsi_get_devices(self):
        if self.is_init:
            for bdev in self.client.bdev_virtio_scsi_get_devices():
                test = Bdev(bdev)
                yield test

    def list_vhost_ctrls(self):
        if self.is_init:
            self.current_vhost_ctrls = self.client.vhost_get_controllers()

    @verbose
    @is_method_available
    def vhost_get_controllers(self, ctrlr_type):
        if self.is_init:
            self.list_vhost_ctrls()
            for ctrlr in [x for x in self.current_vhost_ctrls if ctrlr_type in list(x["backend_specific"].keys())]:
                yield VhostCtrlr(ctrlr)

    @verbose
    def vhost_delete_controller(self, **kwargs):
        self.client.vhost_delete_controller(**kwargs)

    @verbose
    def vhost_create_scsi_controller(self, **kwargs):
        self.client.vhost_create_scsi_controller(**kwargs)

    @verbose
    def vhost_start_scsi_controller(self, **kwargs):
        self.client.vhost_start_scsi_controller(**kwargs)

    @verbose
    def vhost_create_blk_controller(self, **kwargs):
        self.client.vhost_create_blk_controller(**kwargs)

    @verbose
    def vhost_scsi_controller_remove_target(self, **kwargs):
        self.client.vhost_scsi_controller_remove_target(**kwargs)

    @verbose
    def vhost_scsi_controller_add_target(self, **kwargs):
        self.client.vhost_scsi_controller_add_target(**kwargs)

    def vhost_controller_set_coalescing(self, **kwargs):
        self.client.vhost_controller_set_coalescing(**kwargs)

    @verbose
    def create_nvmf_transport(self, **kwargs):
        params = strip_globals(kwargs)
        params = apply_defaults(params, no_srq=False, c2h_success=True)
        params = remove_null(params)
        self.client.nvmf_create_transport(**params)

    def list_nvmf_transports(self):
        if self.is_init:
            self.current_nvmf_transports = self.client.nvmf_get_transports()

    @verbose
    @is_method_available
    def nvmf_get_transports(self):
        if self.is_init:
            self.list_nvmf_transports()
            for transport in self.current_nvmf_transports:
                yield NvmfTransport(transport)

    def list_nvmf_subsystems(self):
        if self.is_init:
            self.current_nvmf_subsystems = self.client.nvmf_get_subsystems()

    @verbose
    @is_method_available
    def nvmf_get_subsystems(self):
        if self.is_init:
            self.list_nvmf_subsystems()
            for subsystem in self.current_nvmf_subsystems:
                yield NvmfSubsystem(subsystem)

    def list_nvmf_referrals(self):
        if self.is_init:
            self.current_nvmf_referrals = self.client.nvmf_discovery_get_referrals()

    @verbose
    @is_method_available
    def nvmf_discovery_get_referrals(self):
        if self.is_init:
            self.list_nvmf_referrals()
            for referral in self.current_nvmf_referrals:
                yield NvmfReferral(referral)

    @verbose
    def nvmf_discovery_add_referral(self, **kwargs):
        params = strip_globals(kwargs)
        params = apply_defaults(params, tgt_name=None)
        params = group_as(params, 'address', ['trtype', 'traddr', 'trsvcid', 'adrfam'])
        if params.get('subnqn') == 'discovery':
            params['subnqn'] = 'nqn.2014-08.org.nvmexpress.discovery'
        self.client.nvmf_discovery_add_referral(**params)

    @verbose
    def nvmf_discovery_remove_referral(self, **kwargs):
        params = strip_globals(kwargs)
        params = group_as(params, 'address', ['trtype', 'traddr', 'trsvcid', 'adrfam'])
        if params.get('subnqn') == 'discovery':
            params['subnqn'] = 'nqn.2014-08.org.nvmexpress.discovery'
        self.client.nvmf_discovery_remove_referral(**params)

    @verbose
    def create_nvmf_subsystem(self, **kwargs):
        self.client.nvmf_create_subsystem(**kwargs)

    @verbose
    def nvmf_delete_subsystem(self, **kwargs):
        self.client.nvmf_delete_subsystem(**kwargs)

    @verbose
    def nvmf_subsystem_add_listener(self, **kwargs):
        params = strip_globals(kwargs)
        params = apply_defaults(params, tgt_name=None)
        params = group_as(params, 'listen_address', ['trtype', 'traddr', 'trsvcid', 'adrfam'])
        if params.get('nqn') == 'discovery':
            params['nqn'] = 'nqn.2014-08.org.nvmexpress.discovery'
        self.client.nvmf_subsystem_add_listener(**params)

    @verbose
    def nvmf_subsystem_remove_listener(self, **kwargs):
        params = strip_globals(kwargs)
        params = group_as(params, 'listen_address', ['trtype', 'traddr', 'trsvcid', 'adrfam'])
        if params.get('nqn') == 'discovery':
            params['nqn'] = 'nqn.2014-08.org.nvmexpress.discovery'
        self.client.nvmf_subsystem_remove_listener(**params)

    @verbose
    def nvmf_subsystem_add_host(self, **kwargs):
        self.client.nvmf_subsystem_add_host(**kwargs)

    @verbose
    def nvmf_subsystem_remove_host(self, **kwargs):
        self.client.nvmf_subsystem_remove_host(**kwargs)

    @verbose
    def nvmf_subsystem_allow_any_host(self, **kwargs):
        self.client.nvmf_subsystem_allow_any_host(**kwargs)

    @verbose
    def nvmf_subsystem_add_ns(self, **kwargs):
        params = strip_globals(kwargs)
        params = apply_defaults(params, tgt_name=None)
        params = group_as(params, 'namespace', ['bdev_name', 'ptpl_file', 'nsid',
                          'nguid', 'eui64', 'uuid', 'anagrpid', 'no_auto_visible', 'hide_metadata'])
        params = remove_null(params)
        self.client.nvmf_subsystem_add_ns(**params)

    @verbose
    def nvmf_subsystem_remove_ns(self, **kwargs):
        self.client.nvmf_subsystem_remove_ns(**kwargs)

    @verbose
    @is_method_available
    def scsi_get_devices(self):
        if self.is_init:
            for device in self.client.scsi_get_devices():
                yield ScsiObj(device)

    @verbose
    @is_method_available
    def iscsi_get_target_nodes(self):
        if self.is_init:
            for tg in self.client.iscsi_get_target_nodes():
                yield tg

    @verbose
    def iscsi_create_target_node(self, **kwargs):
        self.client.iscsi_create_target_node(**kwargs)

    @verbose
    def iscsi_delete_target_node(self, **kwargs):
        self.client.iscsi_delete_target_node(**kwargs)

    @verbose
    @is_method_available
    def iscsi_get_portal_groups(self):
        if self.is_init:
            for pg in self.client.iscsi_get_portal_groups():
                yield ScsiObj(pg)

    @verbose
    @is_method_available
    def iscsi_get_initiator_groups(self):
        if self.is_init:
            for ig in self.client.iscsi_get_initiator_groups():
                yield ScsiObj(ig)

    @verbose
    def construct_portal_group(self, **kwargs):
        self.client.iscsi_create_portal_group(**kwargs)

    @verbose
    def iscsi_delete_portal_group(self, **kwargs):
        self.client.iscsi_delete_portal_group(**kwargs)

    @verbose
    def construct_initiator_group(self, **kwargs):
        self.client.iscsi_create_initiator_group(**kwargs)

    @verbose
    def iscsi_delete_initiator_group(self, **kwargs):
        self.client.iscsi_delete_initiator_group(**kwargs)

    @verbose
    @is_method_available
    def iscsi_get_connections(self, **kwargs):
        if self.is_init:
            for ic in self.client.iscsi_get_connections(**kwargs):
                yield ic

    @verbose
    def iscsi_initiator_group_add_initiators(self, **kwargs):
        self.client.iscsi_initiator_group_add_initiators(**kwargs)

    @verbose
    def iscsi_initiator_group_remove_initiators(self, **kwargs):
        self.client.iscsi_initiator_group_remove_initiators(**kwargs)

    @verbose
    def iscsi_target_node_add_pg_ig_maps(self, **kwargs):
        self.client.iscsi_target_node_add_pg_ig_maps(**kwargs)

    @verbose
    def iscsi_target_node_remove_pg_ig_maps(self, **kwargs):
        self.client.iscsi_target_node_remove_pg_ig_maps(**kwargs)

    @verbose
    def iscsi_auth_group_add_secret(self, **kwargs):
        self.client.iscsi_auth_group_add_secret(**kwargs)

    @verbose
    def iscsi_auth_group_remove_secret(self, **kwargs):
        self.client.iscsi_auth_group_remove_secret(**kwargs)

    @verbose
    @is_method_available
    def iscsi_get_auth_groups(self, **kwargs):
        return self.client.iscsi_get_auth_groups(**kwargs)

    @verbose
    def iscsi_create_auth_group(self, **kwargs):
        self.client.iscsi_create_auth_group(**kwargs)

    @verbose
    def iscsi_delete_auth_group(self, **kwargs):
        self.client.iscsi_delete_auth_group(**kwargs)

    @verbose
    def iscsi_target_node_set_auth(self, **kwargs):
        self.client.iscsi_target_node_set_auth(**kwargs)

    @verbose
    def iscsi_target_node_add_lun(self, **kwargs):
        self.client.iscsi_target_node_add_lun(**kwargs)

    @verbose
    def iscsi_set_discovery_auth(self, **kwargs):
        self.client.iscsi_set_discovery_auth(**kwargs)

    @verbose
    @is_method_available
    def iscsi_get_options(self, **kwargs):
        return self.client.iscsi_get_options(**kwargs)

    def has_subsystem(self, subsystem):
        for system in self.client.framework_get_subsystems():
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


class NvmfReferral(object):
    def __init__(self, referral_info):
        """
        All class attributes are set based on what information is received
        from get_nvmf_referrals RPC call.
        """
        for i in referral_info.keys():
            setattr(self, i, referral_info[i])


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
