from configshell_fb import ConfigNode, ExecutionError
from uuid import UUID
from rpc.client import JSONRPCException
from .ui_node import UINode
import json


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
                          max_namespaces=None, allow_any_host=False):
        """
        Create subsystem with given parameteres.

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
                                                  listen_addresses=listen, hosts=hosts,
                                                  allow_any_host=allow_any_host)
        except JSONRPCException as e:
            self.shell.log.error(e.message)

        self.refresh()

    def ui_command_delete(self, subsystem_nqn=None):
        """
        Delete subsystem qith given nqn. If nqn is empty remove all subsystems

        Arguments:
        nqn_subsystem - Optional argument.
        """
        if subsystem_nqn is None:
            self.shell.log.error("Please specify one of the identifiers: "
                                 "lvol store name or UUID")
        self.get_root().delete_nvmf_subsystem(nqn=subsystem_nqn)
        # self.get_root().refresh()
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

    def refresh_one(self):
        for subsystem in self.get_root().get_nvmf_subsystems():
            if subsystem.nqn == self.subsystem.nqn:
                self.subsystem = subsystem
        self.refresh()

    def ui_command_show_details(self):
        self.shell.log.info(json.dumps(vars(self.lvs), indent=2))

    def ui_command_allow_any_host(self, enable=1):
        """
        Disable or or enable allow_any_host flag.

        Arguments:
        enable - Optional parameter. If 1 then enable, if 0 disable
        """
        enable = self.ui_eval_param(enable, "number", None)
        self.get_root().nvmf_subsystem_allow_any_host(
            nqn=self.subsystem.nqn, disable=True if enable == 0 else False)
        self.get_root().refresh()
        self.refresh_one()

    def summary(self):
        info = ""
        if hasattr(self.subsystem, 'serial_number'):
            info += "sn=%s" % self.subsystem.serial_number
        if hasattr(self.subsystem, 'subtype'):
            if info:
                info += ", "
            info += "st=%s" % self.subsystem.subtype
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

    def refresh_one(self):
        for subsystem in self.get_root().get_nvmf_subsystems():
            if subsystem.nqn == self.parent.subsystem.nqn:
                self.listen_addresses = subsystem.listen_addressed
        self.refresh()

    def ui_command_create(self, trtype, traddr, trsvcid, adrfam):
        """
        Create address listener for subsystem.

        Arguments:
        trtype - NVMe-oF transport type: e.g., rdma.
        traddr - NVMe-oF transport address: e.g., an ip address.
        trsvcid - NVMe-oF transport service id: e.g., a port number.
        adrfam - NVMe-oF transport adrfam: e.g., ipv4, ipv6, ib, fc.
        """
        if trtype is None or traddr is None or trsvcid is None or adrfam is None:
            self.shell.log.error("Please defined all parameters.")
            return
        self.get_root().nvmf_subsystem_add_listener(
             nqn=self.parent.subsystem.nqn, trtype=trtype, traddr=traddr,
             trsvcid=trsvcid, adrfam=adrfam)
        self.get_root().refresh()
        self.refresh_one()

    def ui_command_delete(self, traddr=None, trsvcid=None):
        """
        Remove address listener for subsystem.

        Arguments:
        traddr - NVMe-oF transport address: e.g., an ip address.
        trsvcid - NVMe-oF transport service id: e.g., a port number.
        """
        if (traddr and not trsvcid) or (not traddr and trsvcid):
            self.shell.log.error("Please defined all parameters.")
            return
        if traddr and trsvcid:
            trtype = None
            adrfam = None
            for address in self.listen_addresses:
                if address['traddr'] == traddr and address['trsvcid'] == trsvcid:
                    trtype = address['trtype']
                    adrfam = address['adrfam']
                    break
            self.get_root().nvmf_subsystem_remove_listener(
                nqn=self.parent.subsystem.nqn, trtype=trtype,
                traddr=traddr, trsvcid=trsvcid, adrfam=adrfam)
        else:
            for address in self.listen_addresses:
                self.get_root().nvmf_subsystem_remove_listener(
                    nqn=self.parent.subsystem.nqn, trtype=address['trtype'],
                    traddr=address['traddr'], trsvcid=address['trsvcid'],
                    adrfam=address['adrfam'])
        self.get_root().refresh()
        self.refresh_one()

    def summary(self):
        return "Adresses: %s" % len(self.listen_addresses), None


class UISubsystemListener(UINode):
    def __init__(self, address, parent):
        UINode.__init__(self, "%s:%s" % (address['traddr'], address['trsvcid']),
                        parent)
        self.address = address

    def summary(self):
        return "%s" % self.address['trtype'], True


class UISubsystemHosts(UINode):
    def __init__(self, hosts, parent):
        UINode.__init__(self, "hosts", parent)
        self.hosts = hosts
        self.refresh()

    def refresh(self):
        self._children = set([])
        for host in self.hosts:
            UISubsystemHost(host, self)

    def refresh_one(self):
        for subsystem in self.get_root().get_nvmf_subsystems():
            if subsystem.nqn == self.parent.subsystem.nqn:
                self.hosts = subsystem.hosts
        self.refresh()

    def ui_command_create(self, host):
        self.get_root().nvmf_subsystem_add_host(
             nqn=self.parent.subsystem.nqn, host=host)
        self.get_root().refresh()
        self.refresh_one()

    def ui_command_delete(self, host=None):
        """
        Delete host if defined or all hosts from subsystem.

        Arguments:
        host - Optional parameter. NQN of host to remove.
        """
        if host:
            self.get_root().nvmf_subsystem_remove_host(
                nqn=self.parent.subsystem.nqn, host=host)
        else:
            for host in self.hosts:
                self.get_root().nvmf_subsystem_remove_host(
                   nqn=self.parent.subsystem.nqn, host=host['nqn'])
        self.get_root().refresh()
        self.refresh_one()

    def summary(self):
        return "Host: %s" % len(self.hosts), None


class UISubsystemHost(UINode):
    def __init__(self, host, parent):
        UINode.__init__(self, "%s" % host['nqn'], parent)
        self.host = host

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

    def refresh_one(self):
        for subsystem in self.get_root().get_nvmf_subsystems():
            if subsystem.nqn == self.parent.subsystem.nqn:
                self.namespaces = subsystem.namespaces
        self.refresh()

    def ui_command_create(self, bdev_name):
        try:
            self.get_root().nvmf_subsystem_add_ns(
                nqn=self.parent.subsystem.nqn, bdev_name=bdev_name)
        except Exception as e:
            self.shell.log.error(e.message)
        self.get_root().refresh()
        self.refresh_one()

    def ui_command_delete(self, nsid=None):
        """
        Delete namespace if defined or all namespaces from subsystem.

        Arguments:
        nsid - Optional parameter. Id of namespace to remove.
        """
        if nsid:
            nsid = self.ui_eval_param(nsid, "number", None)
            try:
                self.get_root().nvmf_subsystem_remove_ns(
                    nqn=self.parent.subsystem.nqn, nsid=nsid)
            except Exception as e:
                self.shell.log.error(e.message)
        else:
            for namespace in self.namespaces:
                self.get_root().nvmf_subsystem_remove_ns(
                    nqn=self.parent.subsystem.nqn, nsid=namespace['nsid'])
        self.get_root().refresh()
        self.refresh_one()

    def summary(self):
        return "Namespaces: %s" % len(self.namespaces), None


class UISubsystemNamespace(UINode):
    def __init__(self, namespace, parent):
        UINode.__init__(self, "%s" % namespace['bdev_name'], parent)
        self.namespace = namespace

    def summary(self):
        info = "%s, %s, %s" % (self.namespace['uuid'], self.namespace['name'],
                               self.namespace['nsid'])
        return info, None
