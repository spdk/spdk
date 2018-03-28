from scripts.rpc.client import JSONRPCClient


class NVMfTgt(object):

    def __init__(
            self,
            server='/var/tmp/spdk.sock',
            port=5260):
        super(NVMfTgt, self).__init__()
        self.server_addr = server
        self.port = port
        self.client = JSONRPCClient(self.server_addr, self.port)

    # NVMf-OF
    def get_nvmf_subsystems(self):
        """Display nvmf subsystems

            :return: nvmf subsystems.
        """
        return self.client.call('get_nvmf_subsystems')

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
            params = {
                'nqn': nqn,
                'serial_number': serial_number,
            }
        else:
            raise ValueError('ValueError: nqn and serial_number are empty.')

        if listen:
            params['listen_addresses'] = [dict(u.split(":", 1) for u in a.split(" "))
                                          for a in listen.split(",")]

        if hosts:
            hosts = []
            for u in hosts.strip().split(" "):
                hosts.append(u)
            params['hosts'] = hosts

        if allow_any_host:
            params['allow_any_host'] = True

        if namespaces:
            namespaces = []
            for u in namespaces.strip().split(" "):
                bdev_name = u
                nsid = 0
                if ':' in u:
                    (bdev_name, nsid) = u.split(":")

                ns_params = {'bdev_name': bdev_name}

                nsid = int(nsid)
                if nsid != 0:
                    ns_params['nsid'] = nsid

                namespaces.append(ns_params)
            params['namespaces'] = namespaces

        self.client.call('construct_nvmf_subsystem', params)
        return self.client.call('construct_nvmf_subsystem', params)

    def delete_nvmf_subsystem(self, subsystem_nqn):
        """Delete a nvmf subsystem

            :param subsystem_nqn: subsystem nqn to be deleted.
                                  Example: nqn.2016-06.io.spdk:cnode1.
            :raise: ValueError.
        """
        if subsystem_nqn:
            params = {'nqn': subsystem_nqn}
        else:
            raise ValueError('ValueError: subsystem_nqn is empty.')

        return self.client.call('delete_nvmf_subsystem', params)

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
        params = dict()
        if nqn:
            params['nqn'] = nqn
        else:
            raise ValueError(
                'ValueError: nqn is empty.')

        if (trtype and traddr) and trsvcid:
            listen_address = {'trtype': trtype,
                              'traddr': traddr,
                              'trsvcid': trsvcid}
            params['listen_address'] = listen_address
        else:
            raise ValueError(
                'ValueError: trtype, traddr, trsvcid are empty.')

        if adrfam:
            listen_address['adrfam'] = adrfam

        return self.client.call('nvmf_subsystem_add_listener', params)

    def nvmf_subsystem_add_ns(self, nqn, bdev_name, nsid, nguid, eui64):
        """Add a namespace to an NVMe-oF subsystem

            :param nqn: NVMe-oF subsystem NQN.
            :param bdev_name: The name of the bdev that
                              will back this namespace.
            :param nsid: optional. The requested NSID.
            :param nguid: optional.
            :param eui64: optional.
            :raise: ValueError.
        """
        if nqn:
            params = {'nqn': nqn}
        else:
            raise ValueError('ValueError: nqn is empty.')

        if bdev_name:
            ns = {'bdev_name': bdev_name}
        else:
            raise ValueError('ValueError: bdev_name is empty.')

        if nsid:
            ns['nsid'] = nsid

        if nguid:
            ns['nguid'] = nguid

        if eui64:
            ns['eui64'] = eui64

        params['namespace'] = ns

        return self.client.call('nvmf_subsystem_add_ns', params)
