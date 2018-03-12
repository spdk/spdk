from rpc import nvmf
from rpc.client import JSONRPCClient
from sub_args import SubArgs


class NVMfTgt(object):

    def __init__(
            self,
            server='/var/tmp/spdk.sock',
            port=5260):
        super(NVMfTgt, self).__init__()
        self.sub_args = SubArgs()
        self.sub_args.server_addr = server
        self.sub_args.port = port
        self.sub_args.client = JSONRPCClient(
            self.sub_args.server_addr, self.sub_args.port)

    # NVMf-OF
    def get_nvmf_subsystems(self):
        """Display nvmf subsystems

            :return: nvmf subsystems.
        """
        return nvmf.get_nvmf_subsystems(self.sub_args)

    def construct_nvmf_subsystem(
            self,
            nqn,
            listen,
            hosts,
            allow_any_host,
            namespaces,
            serial_number='0000:00:01.0'):
        """Add a nvmf subsystem

            :param nqn: Target nqn(ASCII).
            :param listen: optional. comma-separated list of Listen.
                           <trtype:transport_name traddr:address trsvcid:port_id>
                           pairs enclosed in quotes. Format:
                           'trtype:transport0 traddr:traddr0 trsvcid:trsvcid0,
                           trtype:transport1 traddr:traddr1 trsvcid:trsvcid1' etc
                           Example: 'trtype:RDMA traddr:192.168.100.8 trsvcid:4420,
                           trtype:RDMA traddr:192.168.100.9 trsvcid:4420'
            :param hosts: optional. Whitespace-separated list of host nqn list.
                          Format: 'nqn1 nqn2' etc. Example:
                          'nqn.2016-06.io.spdk:init nqn.2016-07.io.spdk:init'.
            :param allow_any_host: optional. Allow any host to connect
                                   (don't enforce host NQN whitelist)
            :param namespaces: optional. Whitespace-separated list of
                               namespaces. Format: 'bdev_name1[:nsid1]
                               bdev_name2[:nsid2] bdev_name3[:nsid3]' etc.
                               Example: '1:Malloc0 2:Malloc1 3:Malloc2'
                              *** The devices must pre-exist ***
            :param serial_number: Format: 'sn' etc. Example: 'SPDK00000000000001'.
            :raise: ValueError.
        """
        if nqn and serial_number:
            self.sub_args.nqn = nqn
            self.sub_args.serial_number = serial_number
        else:
            raise ValueError('ValueError: nqn and serial_number are empty.')
        self.sub_args.listen = listen
        self.sub_args.hosts = hosts
        self.sub_args.allow_any_host = allow_any_host
        self.sub_args.namespaces = namespaces
        nvmf.construct_nvmf_subsystem(self.sub_args)

    def delete_nvmf_subsystem(self, subsystem_nqn):
        """Delete a nvmf subsystem

            :param subsystem_nqn: subsystem nqn to be deleted.
                                  Example: nqn.2016-06.io.spdk:cnode1.
            :raise: ValueError.
        """
        if subsystem_nqn:
            self.sub_args.subsystem_nqn = subsystem_nqn
        else:
            raise ValueError('ValueError: subsystem_nqn is empty.')
        nvmf.delete_nvmf_subsystem(self.sub_args)

    def nvmf_subsystem_add_listener(
            self,
            nqn,
            trtype,
            traddr,
            adrfam,
            trsvcid):
        """Add a listener to an NVMe-oF subsystem

            :param nqn: NVMe-oF subsystem NQN.
            :param trtype: NVMe-oF transport type:
                           e.g., rdma.
            :param traddr: NVMe-oF transport address:
                           e.g., an ip address.
            :param adrfam: optional. NVMe-oF transport adrfam:
                           e.g., ipv4, ipv6, ib, fc, intra_host.
            :param trsvcid: NVMe-oF transport service id:
                           e.g., a port number.
            :raise: ValueError.
        """
        if (nqn and trtype) and (traddr and trsvcid):
            self.sub_args.nqn = nqn
            self.sub_args.trtype = trtype
            self.sub_args.traddr = traddr
            self.sub_args.trsvcid = trsvcid
        else:
            raise ValueError(
                'ValueError: nqn, trtype, traddr, trsvcid are empty.')
        self.sub_args.adrfam = adrfam
        nvmf.nvmf_subsystem_add_listener(self.sub_args)

    def nvmf_subsystem_add_ns(self, nqn, bdev_name, nsid):
        """Add a namespace to an NVMe-oF subsystem

            :param nqn: NVMe-oF subsystem NQN.
            :param bdev_name: The name of the bdev that
                              will back this namespace.
            :param nsid: optional. The requested NSID.
            :raise: ValueError.
        """
        if nqn and bdev_name:
            self.sub_args.nqn = nqn
            self.sub_args.bdev_name = bdev_name
        else:
            raise ValueError('ValueError: nqn and bdev_name are empty.')
        self.sub_args.nsid = nsid
        nvmf.nvmf_subsystem_add_ns(self.sub_args)
