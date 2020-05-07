from rpc.client import JSONRPCException
from .ui_node import UINode


class UINVMf(UINode):
    def __init__(self, parent):
        UINode.__init__(self, "nvmf", parent)
        self.refresh()

    def refresh(self):
        self._children = set([])
        UINVMfSubsystems(self)
        UINVMfTransports(self)


class UINVMfTransports(UINode):
    def __init__(self, parent):
        UINode.__init__(self, "transport", parent)
        self.refresh()

    def refresh(self):
        self._children = set([])
        for transport in self.get_root().nvmf_get_transports():
            UINVMfTransport(transport, self)

    def ui_command_create(self, trtype, max_queue_depth=None, max_io_qpairs_per_ctrlr=None,
                          in_capsule_data_size=None, max_io_size=None, io_unit_size=None, max_aq_depth=None):
        """Create a transport with given parameters

        Arguments:
            trtype - Example: 'RDMA'.
            max_queue_depth - Optional parameter. Integer, max value 65535.
            max_io_qpairs_per_ctrlr - Optional parameter. 16 bit Integer, max value 65535.
            in_capsule_data_size - Optional parameter. 32 bit Integer, max value 4294967295
            max_io_size - Optional parameter. 32 bit integer, max value 4294967295
            io_unit_size - Optional parameter. 32 bit integer, max value 4294967295
            max_aq_depth - Optional parameter. 32 bit integer, max value 4294967295
        """
        max_queue_depth = self.ui_eval_param(max_queue_depth, "number", None)
        max_io_qpairs_per_ctrlr = self.ui_eval_param(max_io_qpairs_per_ctrlr, "number", None)
        in_capsule_data_size = self.ui_eval_param(in_capsule_data_size, "number", None)
        max_io_size = self.ui_eval_param(max_io_size, "number", None)
        io_unit_size = self.ui_eval_param(io_unit_size, "number", None)
        max_aq_depth = self.ui_eval_param(max_aq_depth, "number", None)

        self.get_root().create_nvmf_transport(trtype=trtype,
                                              max_queue_depth=max_queue_depth,
                                              max_io_qpairs_per_ctrlr=max_io_qpairs_per_ctrlr,
                                              in_capsule_data_size=in_capsule_data_size,
                                              max_io_size=max_io_size,
                                              io_unit_size=io_unit_size,
                                              max_aq_depth=max_aq_depth)

    def summary(self):
        return "Transports: %s" % len(self.children), None


class UINVMfTransport(UINode):
    def __init__(self, transport, parent):
        UINode.__init__(self, transport.trtype, parent)
        self.transport = transport


class UINVMfSubsystems(UINode):
    def __init__(self, parent):
        UINode.__init__(self, "subsystem", parent)
        self.refresh()

    def refresh(self):
        self._children = set([])
        for subsystem in self.get_root().nvmf_get_subsystems():
            UINVMfSubsystem(subsystem, self)

    def delete(self, subsystem_nqn):
        self.get_root().nvmf_delete_subsystem(nqn=subsystem_nqn)

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
        self.get_root().create_nvmf_subsystem(nqn=nqn, serial_number=serial_number,
                                              allow_any_host=allow_any_host,
                                              max_namespaces=max_namespaces)

    def ui_command_delete(self, subsystem_nqn):
        """Delete subsystem with given nqn.

        Arguments:
            nqn_subsystem - Name of susbsytem to delete
        """
        self.delete(subsystem_nqn)

    def ui_command_delete_all(self):
        """Delete all subsystems"""
        rpc_messages = ""
        for child in self._children:
            try:
                self.delete(child.subsystem.nqn)
            except JSONRPCException as e:
                rpc_messages += e.message
        if rpc_messages:
            raise JSONRPCException(rpc_messages)

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
        for subsystem in self.get_root().nvmf_get_subsystems():
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
        self.get_root().nvmf_subsystem_allow_any_host(
            nqn=self.subsystem.nqn, disable=disable)

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
        for subsystem in self.get_root().nvmf_get_subsystems():
            if subsystem.nqn == self.parent.subsystem.nqn:
                self.listen_addresses = subsystem.listen_addresses
        self.refresh()

    def delete(self, trtype, traddr, trsvcid, adrfam=None):
        self.get_root().nvmf_subsystem_remove_listener(
            nqn=self.parent.subsystem.nqn, trtype=trtype,
            traddr=traddr, trsvcid=trsvcid, adrfam=adrfam)

    def ui_command_create(self, trtype, traddr, trsvcid, adrfam):
        """Create address listener for subsystem.

        Arguments:
            trtype - NVMe-oF transport type: e.g., rdma.
            traddr - NVMe-oF transport address: e.g., an ip address.
            trsvcid - NVMe-oF transport service id: e.g., a port number.
            adrfam - NVMe-oF transport adrfam: e.g., ipv4, ipv6, ib, fc.
        """
        self.get_root().nvmf_subsystem_add_listener(
            nqn=self.parent.subsystem.nqn, trtype=trtype, traddr=traddr,
            trsvcid=trsvcid, adrfam=adrfam)

    def ui_command_delete(self, trtype, traddr, trsvcid, adrfam=None):
        """Remove address listener for subsystem.

        Arguments:
            trtype - Transport type (RDMA)
            traddr - NVMe-oF transport address: e.g., an ip address.
            trsvcid - NVMe-oF transport service id: e.g., a port number.
            adrfam - Optional argument. Address family ("IPv4", "IPv6", "IB" or "FC").
        """
        self.delete(trtype, traddr, trsvcid, adrfam)

    def ui_command_delete_all(self):
        """Remove all address listeners from subsystem."""
        rpc_messages = ""
        for la in self.listen_addresses:
            try:
                self.delete(la['trtype'], la['traddr'], la['trsvcid'], la['adrfam'])
            except JSONRPCException as e:
                rpc_messages += e.message
        if rpc_messages:
            raise JSONRPCException(rpc_messages)

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
        for subsystem in self.get_root().nvmf_get_subsystems():
            if subsystem.nqn == self.parent.subsystem.nqn:
                self.hosts = subsystem.hosts
        self.refresh()

    def delete(self, host):
        self.get_root().nvmf_subsystem_remove_host(
            nqn=self.parent.subsystem.nqn, host=host)

    def ui_command_create(self, host):
        """Add a host NQN to the whitelist of allowed hosts.

        Args:
            host: Host NQN to add to the list of allowed host NQNs
        """
        self.get_root().nvmf_subsystem_add_host(
            nqn=self.parent.subsystem.nqn, host=host)

    def ui_command_delete(self, host):
        """Delete host from subsystem.

        Arguments:
           host - NQN of host to remove.
        """
        self.delete(host)

    def ui_command_delete_all(self):
        """Delete host from subsystem"""
        rpc_messages = ""
        for host in self.hosts:
            try:
                self.delete(host['nqn'])
            except JSONRPCException as e:
                rpc_messages += e.message
        if rpc_messages:
            raise JSONRPCException(rpc_messages)

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
        for subsystem in self.get_root().nvmf_get_subsystems():
            if subsystem.nqn == self.parent.subsystem.nqn:
                self.namespaces = subsystem.namespaces
        self.refresh()

    def delete(self, nsid):
        self.get_root().nvmf_subsystem_remove_ns(
            nqn=self.parent.subsystem.nqn, nsid=nsid)

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
        self.get_root().nvmf_subsystem_add_ns(
            nqn=self.parent.subsystem.nqn, bdev_name=bdev_name,
            nsid=nsid, nguid=nguid, eui64=eui64, uuid=uuid)

    def ui_command_delete(self, nsid):
        """Delete namespace from subsystem.

        Arguments:
            nsid - Id of namespace to remove.
        """
        nsid = self.ui_eval_param(nsid, "number", None)
        self.delete(nsid)

    def ui_command_delete_all(self):
        """Delete all namespaces from subsystem."""
        rpc_messages = ""
        for namespace in self.namespaces:
            try:
                self.delete(namespace['nsid'])
            except JSONRPCException as e:
                rpc_messages += e.message
        if rpc_messages:
            raise JSONRPCException(rpc_messages)

    def summary(self):
        return "Namespaces: %s" % len(self.namespaces), None


class UINVMfSubsystemNamespace(UINode):
    def __init__(self, namespace, parent):
        UINode.__init__(self, namespace['bdev_name'], parent)
        self.namespace = namespace

    def summary(self):
        info = ", ".join([self.namespace['name'], str(self.namespace['nsid'])])
        return info, None
