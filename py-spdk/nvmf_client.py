import json
from proto import spdk_pb2
from util import pbjson


class NvmfTgt(object):

    def __init__(self, py):
        super(NvmfTgt, self).__init__()
        self.py = py

    def get_rpc_methods(self):
        res = self.py.exec_rpc('get_rpc_methods', '10.0.2.15')
        json_obj = json.loads(res)
        proto_obj = spdk_pb2.RpcMethods().name
        for name_i in range(len(json_obj)):
            proto_obj.append(json_obj[name_i])
        return proto_obj

    def get_bdevs(self):
        block_devices = spdk_pb2.BlockDevices().blockDevice
        block_device = spdk_pb2.BlockDevice
        block_devices = self.get_proto_objs(
            'get_bdevs', '10.0.2.15', block_devices, block_device)
        return block_devices

    def delete_bdev(self, name):
        sub_args = []
        sub_args.append(name)
        res = self.py.exec_rpc('delete_bdev', '10.0.2.15', sub_args=sub_args)
        print res

    def kill_instance(self, sig_name):
        sub_args = []
        sub_args.append(sig_name)
        res = self.py.exec_rpc('kill_instance', '10.0.2.15', sub_args=sub_args)
        print res

    def construct_aio_bdev(self, filename, name, block_size):
        sub_args = []
        sub_args.append(filename)
        sub_args.append(name)
        sub_args.append(str(block_size))
        res = self.py.exec_rpc('construct_aio_bdev', '10.0.2.15', sub_args=sub_args)
        print res

    def construct_error_bdev(self, basename):
        sub_args = []
        sub_args.append(basename)
        res = self.py.exec_rpc(
            'construct_error_bdev',
            '10.0.2.15',
            sub_args=sub_args)
        print res

    def construct_nvme_bdev(
            self,
            name,
            trtype,
            traddr,
            adrfam=None,
            trsvcid=None,
            subnqn=None):
        sub_args = ["-b", "-t", "-a"]
        sub_args.insert(1, name)
        sub_args.insert(2, trtype)
        sub_args.insert(3, traddr)
        if adrfam is not None:
            sub_args.append("-f")
            sub_args.append(adrfam)
        if trsvcid is not None:
            sub_args.append("-s")
            sub_args.append(trsvcid)
        if subnqn is not None:
            sub_args.append("-n")
            sub_args.append(subnqn)
        res = self.py.exec_rpc(
            'construct_nvme_bdev',
            '10.0.2.15',
            sub_args=sub_args)
        print res

    def construct_null_bdev(self, name, total_size, block_size):
        sub_args = []
        sub_args.append(name)
        sub_args.append(str(total_size))
        sub_args.append(str(block_size))
        res = self.py.exec_rpc(
            'construct_null_bdev',
            '10.0.2.15',
            sub_args=sub_args)
        print res

    def construct_malloc_bdev(self, total_size, block_size):
        sub_args = []
        sub_args.append(str(total_size))
        sub_args.append(str(block_size))
        res = self.py.exec_rpc(
            'construct_malloc_bdev',
            '10.0.2.15',
            sub_args=sub_args)
        print res

    def delete_nvmf_subsystem(self, nqn):
        sub_args = []
        sub_args.append(nqn)
        res = self.py.exec_rpc(
            'delete_nvmf_subsystem',
            '10.0.2.15',
            sub_args=sub_args)
        print res

    def construct_nvmf_subsystem(
            self,
            nqn,
            listen,
            hosts,
            serial_number,
            namespaces):
        sub_args = []
        sub_args.append(nqn)
        sub_args.append(listen)
        sub_args.append(hosts)
        sub_args.append(serial_number)
        sub_args.append(namespaces)
        res = self.py.exec_rpc(
            'construct_nvmf_subsystem',
            '10.0.2.15',
            sub_args=sub_args)
        print res

    def get_nvmf_subsystems(self):
        subsystems = spdk_pb2.NVMFSubsystems().subsystem
        subsystem = spdk_pb2.NVMFSubsystem
        subsystems = self.get_proto_objs(
            'get_nvmf_subsystems', '10.0.2.15', subsystems, subsystem)
        return subsystems

    def get_proto_objs(self, method, server_ip, proto_objs, proto_obj):
        res = self.py.exec_rpc(method, server_ip)
        json_obj = json.loads(res)
        for list_i in range(len(json_obj)):
            vproto_obj = pbjson.dict2pb(proto_obj, json_obj[list_i])
            proto_objs.extend([vproto_obj])
        return proto_objs
