from rpc.client import JSONRPCException
from .ui_node import UINode


class UINVMf(UINode):
    def __init__(self, parent):
        UINode.__init__(self, "nvmf", parent)
        self.refresh()

    def refresh(self):
        self._children = set([])
        UINVMfSubsystems(self)


class UINVMfSubsystems(UINode):
    def __init__(self, parent):
        UINode.__init__(self, "subsystem", parent)
        self.refresh()

    def refresh(self):
        self._children = set([])
        for subsystem in self.get_root().get_nvmf_subsystems():
            UINVMfSubsystem(subsystem, self)

    def ui_command_create(self, nqn, serial_number=None,
                          max_namespaces=None, allow_any_host="false"):
        """Create subsystem with given parameteres.

        Arguments:
            nqn - Target nqn(ASCII).
            serial_number - Example: 'SPDK00000000000001'.
            max_namespaces - Optional parameter. Maximum number of namespaces allowed to added during
                             active connection
            allow_any_host - Optional parameter. Allow any host to connect (don't enforce host NQN
                             whitelist)
        """
        allow_any_host = self.ui_eval_param(allow_any_host, "bool", False)
        max_namespaces = self.ui_eval_param(max_namespaces, "number", 0)
        try:
            self.get_root().create_nvmf_subsystem(nqn=nqn, serial_number=serial_number,
                                                  allow_any_host=allow_any_host,
                                                  max_namespaces=max_namespaces)
        except JSONRPCException as e:
            self.shell.log.error(e.message)
        self.refresh()

    def ui_command_delete(self, subsystem_nqn):
        """Delete subsystem with given nqn.

        Arguments:
            nqn_subsystem - Name of susbsytem to delete
        """
        try:
            self.get_root().delete_nvmf_subsystem(nqn=subsystem_nqn)
        except JSONRPCException as e:
            self.shell.log.error(e.message)
        self.refresh()

    def summary(self):
        return "Subsystems: %s" % len(self.children), None


class UINVMfSubsystem(UINode):
    def __init__(self, subsystem, parent):
        UINode.__init__(self, subsystem.nqn, parent)
        self.subsystem = subsystem
        self.refresh()

    def refresh(self):
        self._children = set([])
        UINVMfSubsystemListeners(self.subsystem.listen_addresses, self)
        UINVMfSubsystemHosts(self.subsystem.hosts, self)
        if hasattr(self.subsystem, 'namespaces'):
            UINVMfSubsystemNamespaces(self.subsystem.namespaces, self)

    def refresh_node(self):
        for subsystem in self.get_root().get_nvmf_subsystems():
            if subsystem.nqn == self.subsystem.nqn:
                self.subsystem = subsystem
        self.refresh()

    def ui_command_show_details(self):
        self.shell.log.info(json.dumps(vars(self.lvs), indent=2))

    def ui_command_allow_any_host(self, disable="false"):
        """Disable or or enable allow_any_host flag.

        Arguments:
            disable - Optional parameter. If false then enable, if true disable
        """
        disable = self.ui_eval_param(disable, "bool", None)
        try:
            self.get_root().nvmf_subsystem_allow_any_host(
                nqn=self.subsystem.nqn, disable=disable)
        except JSONRPCException as e:
            self.shell.log.error(e.message)
        self.get_root().refresh()
        self.refresh_node()

    def summary(self):
        sn = None
        if hasattr(self.subsystem, 'serial_number'):
            sn = "sn=%s" % self.subsystem.serial_number
        st = None
        if hasattr(self.subsystem, 'subtype'):
            st = "st=%s" % self.subsystem.subtype
        allow_any_host = None
        if self.subsystem.allow_any_host:
            allow_any_host = "Allow any host"
        info = ", ".join(filter(None, [sn, st, allow_any_host]))
        return info, None


class UINVMfSubsystemListeners(UINode):
    def __init__(self, listen_addresses, parent):
        UINode.__init__(self, "listen_addresses", parent)
        self.listen_addresses = listen_addresses
        self.refresh()

    def refresh(self):
        self._children = set([])
        for address in self.listen_addresses:
            UINVMfSubsystemListener(address, self)

    def refresh_node(self):
        for subsystem in self.get_root().get_nvmf_subsystems():
            if subsystem.nqn == self.parent.subsystem.nqn:
                self.listen_addresses = subsystem.listen_addresses
        self.refresh()

    def ui_command_create(self, trtype, traddr, trsvcid, adrfam):
        """Create address listener for subsystem.

        Arguments:
            trtype - NVMe-oF transport type: e.g., rdma.
            traddr - NVMe-oF transport address: e.g., an ip address.
            trsvcid - NVMe-oF transport service id: e.g., a port number.
            adrfam - NVMe-oF transport adrfam: e.g., ipv4, ipv6, ib, fc.
        """
        try:
            self.get_root().nvmf_subsystem_add_listener(
                nqn=self.parent.subsystem.nqn, trtype=trtype, traddr=traddr,
                trsvcid=trsvcid, adrfam=adrfam)
        except JSONRPCException as e:
            self.shell.log.error(e.message)
        self.get_root().refresh()
        self.refresh_node()

    def ui_command_delete(self, trtype, traddr, trsvcid, adrfam=None):
        """Remove address listener for subsystem.

        Arguments:
            trtype - Transport type (RDMA)
            traddr - NVMe-oF transport address: e.g., an ip address.
            trsvcid - NVMe-oF transport service id: e.g., a port number.
            adrfam - Optional argument. Address family ("IPv4", "IPv6", "IB" or "FC").
        """
        try:
            self.get_root().nvmf_subsystem_remove_listener(
                nqn=self.parent.subsystem.nqn, trtype=trtype,
                traddr=traddr, trsvcid=trsvcid, adrfam=adrfam)
        except JSONRPCException as e:
            self.shell.log.error(e.message)
        self.get_root().refresh()
        self.refresh_node()

    def summary(self):
        return "Addresses: %s" % len(self.listen_addresses), None


class UINVMfSubsystemListener(UINode):
    def __init__(self, address, parent):
        UINode.__init__(self, "%s:%s" % (address['traddr'], address['trsvcid']),
                        parent)
        self.address = address

    def summary(self):
        return "%s" % self.address['trtype'], True


class UINVMfSubsystemHosts(UINode):
    def __init__(self, hosts, parent):
        UINode.__init__(self, "hosts", parent)
        self.hosts = hosts
        self.refresh()

    def refresh(self):
        self._children = set([])
        for host in self.hosts:
            UINVMfSubsystemHost(host, self)

    def refresh_node(self):
        for subsystem in self.get_root().get_nvmf_subsystems():
            if subsystem.nqn == self.parent.subsystem.nqn:
                self.hosts = subsystem.hosts
        self.refresh()

    def ui_command_create(self, host):
        """Add a host NQN to the whitelist of allowed hosts.

        Args:
            host: Host NQN to add to the list of allowed host NQNs
        """
        try:
            self.get_root().nvmf_subsystem_add_host(
                nqn=self.parent.subsystem.nqn, host=host)
        except JSONRPCException as e:
            self.shell.log.error(e.message)
        self.get_root().refresh()
        self.refresh_node()

    def ui_command_delete(self, host):
        """Delete host from subsystem.

        Arguments:
           host - NQN of host to remove.
        """
        try:
            self.get_root().nvmf_subsystem_remove_host(
                nqn=self.parent.subsystem.nqn, host=host)
        except JSONRPCException as e:
            self.shell.log.error(e.message)
        self.get_root().refresh()
        self.refresh_node()

    def summary(self):
        return "Hosts: %s" % len(self.hosts), None


class UINVMfSubsystemHost(UINode):
    def __init__(self, host, parent):
        UINode.__init__(self, "%s" % host['nqn'], parent)
        self.host = host


class UINVMfSubsystemNamespaces(UINode):
    def __init__(self, namespaces, parent):
        UINode.__init__(self, "namespaces", parent)
        self.namespaces = namespaces
        self.refresh()

    def refresh(self):
        self._children = set([])
        for namespace in self.namespaces:
            UINVMfSubsystemNamespace(namespace, self)

    def refresh_node(self):
        for subsystem in self.get_root().get_nvmf_subsystems():
            if subsystem.nqn == self.parent.subsystem.nqn:
                self.namespaces = subsystem.namespaces
        self.refresh()

    def ui_command_create(self, bdev_name, nsid=None,
                          nguid=None, eui64=None, uuid=None):
        """Add a namespace to a subsystem.

        Args:
            bdev_name: Name of bdev to expose as a namespace.
        Optional args:
            nsid: Namespace ID.
            nguid: 16-byte namespace globally unique identifier in hexadecimal.
            eui64: 8-byte namespace EUI-64 in hexadecimal (e.g. "ABCDEF0123456789").
            uuid: Namespace UUID.
        """
        nsid = self.ui_eval_param(nsid, "number", None)
        try:
            self.get_root().nvmf_subsystem_add_ns(
                nqn=self.parent.subsystem.nqn, bdev_name=bdev_name,
                nsid=nsid, nguid=nguid, eui64=eui64, uuid=uuid)
        except JSONRPCException as e:
            self.shell.log.error(e.message)
        self.get_root().refresh()
        self.refresh_node()

    def ui_command_delete(self, nsid):
        """Delete namespace from subsystem.

        Arguments:
            nsid - Id of namespace to remove.
        """
        nsid = self.ui_eval_param(nsid, "number", None)
        try:
            self.get_root().nvmf_subsystem_remove_ns(
                nqn=self.parent.subsystem.nqn, nsid=nsid)
        except JSONRPCException as e:
            self.shell.log.error(e.message)
        self.get_root().refresh()
        self.refresh_node()

    def summary(self):
        return "Namespaces: %s" % len(self.namespaces), None


class UINVMfSubsystemNamespace(UINode):
    def __init__(self, namespace, parent):
        UINode.__init__(self, namespace['bdev_name'], parent)
        self.namespace = namespace

    def summary(self):
        info = ", ".join([str(self.namespace['uuid']), self.namespace['name'],
                          str(self.namespace['nsid'])])
        return info, None
