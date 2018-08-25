from configshell_fb import ConfigNode, ExecutionError
from uuid import UUID
from rpc.client import JSONRPCException
from ui_node import UINode
import json
import ui_node

class UINvmf(UINode):
    def __init__(self, parent):
        UINode.__init__(self, "nvmf", parent)
        self.refresh()

    def refresh(self):
        self._children = set([])
        UISubsystems(self)


class UISubsystems(UINode):
    def __init__(self, parent):
        UINode.__init__(self, "subsystem", parent)
        self.refresh()

    def refresh(self):
        self._children = set([])
        for subsystem in self.get_root().get_nvmf_subsystems():
            UISubsystem(subsystem, self)

    def ui_command_create(self, nqn, serial_number, listen=None, hosts=None, namespaces=None,
                          max_namespaces=None):
        """
        Creates logical volume store on target bdev.

        Arguments:
        name - Friendly name to use alongside with UUID identifier.
        bdev_name - On which bdev to create the lvol store.
        cluster_size - Cluster size to use when creating lvol store, in bytes. Default: 4194304.
        """
        if listen:
            listen = " ".join(listen.split("-"))
            listen = [
                dict(
                    u.split(
                        ":",
                        1) for u in a.split(" ")) for a in listen.split(",")]
        try:
            self.get_root().create_nvmf_subsystem(nqn=nqn, serial_number=serial_number,
                                                  listen_addresses=listen, hosts=hosts)
        except JSONRPCException as e:
            self.shell.log.error(e.message)

        self.get_root().refresh()
        self.refresh()

    def ui_command_delete(self, subsystem_nqn=None):
        """
        Deletes logical volume store from configuration.
        This will also delete all logical volume bdevs created on this lvol store!

        Arguments:
        name - Friendly name of the logical volume store to be deleted.
        uuid - UUID number of the logical volume store to be deleted.
        """
        if subsystem_nqn is None:
            self.shell.log.error("Please specify one of the identifiers: "
                                 "lvol store name or UUID")
        self.get_root().delete_nvmf_subsystem(nqn=subsystem_nqn)
        self.get_root().refresh()
        self.refresh()

    def summary(self):
        return "Subsystems: %s" % len(self.children), None


class UISubsystem(UINode):
    def __init__(self, subsystem, parent):
        UINode.__init__(self, subsystem.nqn, parent)
        self.subsystem = subsystem
        self.refresh()

    def refresh(self):
        self._children = set([])
        UISubsystemListeners(self.subsystem.listen_addresses, self)
        UISubsystemHosts(self.subsystem.hosts, self)
        if hasattr(self.subsystem, 'namespaces'):
            UISubsystemNamespaces(self.subsystem.namespaces, self)

    def ui_command_show_details(self):
        self.shell.log.info(json.dumps(vars(self.lvs), indent=2))

    def summary(self):
        info = ""
        if hasattr(self.subsystem, 'serial_number'):
            info += "%s" % self.subsystem.serial_number
        if hasattr(self.subsystem, 'subtype'):
            if info:
                info += ", "
            info += "%s" % self.subsystem.subtype
        if self.subsystem.allow_any_host:
            if info:
                info += ", "
            info += "Allow any host"
        return info, None


class UISubsystemListeners(UINode):
    def __init__(self, listen_addresses, parent):
        UINode.__init__(self, "listen_addresses", parent)
        self.listen_addresses = listen_addresses
        self.refresh()

    def refresh(self):
        self._children = set([])
        for address in self.listen_addresses:
            UISubsystemListener(address, self)

    def ui_command_create(self, trtype, traddr, trsvcid, adrfam):
        self.get_root().nvmf_subsystem_add_listener(
             nqn=self.parent.subsystem.nqn, trtype=trtype, traddr=traddr,
             trsvcid=trsvcid, adrfam=adrfam)
        self.get_root().refresh()
        self.refresh()

    def ui_command_delete(self):
        for address in self.listen_address:
            self.get_root().nvmf_subsystem_remove_listener(
                nqn=self.parent.subsystem.nqn, trtype=address['trtype'],
                traddr=address['traddr'], trsvcid=address['trsvcid'],
                adrfam=address['adrfam'])
        self.get_root().refresh()
        self.refresh()

    def summary(self):
        return "Adresses: %s" % len(self.listen_addresses), None


class UISubsystemListener(UINode):
    def __init__(self, address, parent):
        UINode.__init__(self, "trtype: %s, traddr: %s, trsvcid: %s" % (
                        address['trtype'], address['traddr'], address['trsvcid']),
                        parent)
        self.address = address

    def ui_command_delete(self):
        self.get_root().nvmf_subsystem_remove_listener(
             nqn=self.parent.parent.subsystem.nqn, trtype=address['trtype'],
             traddr=address['traddr'], trsvcid=address['trsvcid'],
             adrfam=address['adrfam'])
        self.get_root().refresh()
        self.refresh()

    def summary(self):
        return "", True


class UISubsystemHosts(UINode):
    def __init__(self, hosts, parent):
        UINode.__init__(self, "hosts", parent)
        self.hosts = hosts
        self.refresh()

    def refresh(self):
        self._children = set([])
        for host in self.hosts:
            UISubsystemListener(host, self)

    def ui_command_create(self, host):
        self.get_root().nvmf_subsystem_add_host(
             nqn=self.parent.parent.subsystem.nqn, host=self.host)
        self.get_root().refresh()
        self.refresh()

    def ui_command_delete(self):
        for host in self.hosts:
            self.get_root().nvmf_subsystem_remove_host(
                nqn=self.parent.subsystem.nqn, host=host)
        self.get_root().refresh()
        self.refresh()

    def summary(self):
        return "Host: %s" % len(self.hosts), None


class UISubsystemHost(UINode):
    def __init__(self, host, parent):
        UINode.__init__(self, "%s" % host, parent)
        self.host = host

    def ui_command_delete(self):
        self.get_root().nvmf_subsystem_remove_host(
            nqn=self.parent.parent.subsystem.nqn, host=self.host)
        self.get_root().refresh()
        self.refresh()

    def summary(self):
        return " ", None


class UISubsystemNamespaces(UINode):
    def __init__(self, namespaces, parent):
        UINode.__init__(self, "namespaces", parent)
        self.namespaces = namespaces
        self.refresh()

    def refresh(self):
        self._children = set([])
        for namespace in self.namespaces:
            UISubsystemNamespace(namespace, self)

    def ui_command_create(self, host):
        self.get_root().nvmf_subsystem_add_ns(
             nqn=self.parent.parent.subsystem.nqn, host=self.host)
        self.get_root().refresh()
        self.refresh()

    def ui_command_delete(self):
        for nsid in self.namespaces:
            self.get_root().nvmf_subsystem_remove_ns(
                nqn=self.parent.subsystem.nqn, nsid=nsid)
        self.get_root().refresh()
        self.refresh()

    def summary(self):
        return "Namespaces: %s" % len(self.namespaces), None


class UISubsystemNamespace(UINode):
    def __init__(self, namespace, parent):
        UINode.__init__(self, "%s" % namespace, parent)
        self.namespace = namespace

    def ui_command_delete(self):
        self.get_root().nvmf_subsystem_remove_ns(
            nqn=self.parent.parent.subsystem.nqn, nsid=self.namespace)
        self.get_root().refresh()
        self.refresh()

    def summary(self):
        return " ", None

