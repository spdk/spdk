import json

from proto import spdk_pb2
from util import pbjson


class VhostTgt(object):

    def __init__(self, py):
        super(VhostTgt, self).__init__()
        self.py = py

    def get_rpc_methods(self):
        res = self.py.exec_rpc('get_rpc_methods', '127.0.0.1')
        json_obj = json.loads(res)
        proto_obj = spdk_pb2.RpcMethods().name
        for name_i in range(len(json_obj)):
            proto_obj.append(json_obj[name_i])
        return proto_obj

    def get_scsi_devices(self):
        scsi_devices = spdk_pb2.ScsiDevices().scsiDevice
        scsi_device = spdk_pb2.ScsiDevice
        scsi_devices = self._get_proto_objs(
            'get_scsi_devices', '127.0.0.1', scsi_devices, scsi_device)
        return scsi_devices

    def get_luns(self):
        luns = spdk_pb2.Luns().lun
        lun = spdk_pb2.Lun
        luns = self._get_proto_objs('get_luns', '127.0.0.1', luns, lun)
        return luns

    def get_interfaces(self):
        interfaces = spdk_pb2.Interfaces().interface
        interface = spdk_pb2.Interface
        interfaces = self._get_proto_objs(
            'get_interfaces', '127.0.0.1', interfaces, interface)
        return interfaces

    def add_ip_address(self, ifc_index, ip_addr):
        sub_args = []
        sub_args.append(ifc_index)
        sub_args.append(ip_addr)
        res = self.py.exec_rpc('add_ip_address', '127.0.0.1', sub_args=sub_args)
        print res

    def delete_ip_address(self, ifc_index, ip_addr):
        sub_args = []
        sub_args.append(ifc_index)
        sub_args.append(ip_addr)
        res = self.py.exec_rpc('delete_ip_address', '127.0.0.1', sub_args=sub_args)
        print res

    def get_bdevs(self):
        block_devices = spdk_pb2.BlockDevices().blockDevice
        block_device = spdk_pb2.BlockDevice
        block_devices = self._get_proto_objs(
            'get_bdevs', '127.0.0.1', block_devices, block_device)
        return block_devices

    def delete_bdev(self, name):
        sub_args = []
        sub_args.append(name)
        res = self.py.exec_rpc('delete_bdev', '127.0.0.1', sub_args=sub_args)
        print res

    def kill_instance(self, sig_name):
        sub_args = []
        sub_args.append(sig_name)
        res = self.py.exec_rpc('kill_instance', '127.0.0.1', sub_args=sub_args)
        print res

    def construct_aio_bdev(self, filename, name, block_size):
        sub_args = []
        sub_args.append(filename)
        sub_args.append(name)
        sub_args.append(str(block_size))
        res = self.py.exec_rpc('construct_aio_bdev', '127.0.0.1', sub_args=sub_args)
        print res

    def construct_error_bdev(self, basename):
        sub_args = []
        sub_args.append(basename)
        res = self.py.exec_rpc(
            'construct_error_bdev',
            '127.0.0.1',
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
            '127.0.0.1',
            sub_args=sub_args)
        print res

    def construct_null_bdev(self, name, total_size, block_size):
        sub_args = []
        sub_args.append(name)
        sub_args.append(str(total_size))
        sub_args.append(str(block_size))
        res = self.py.exec_rpc(
            'construct_null_bdev',
            '127.0.0.1',
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

    def _get_proto_objs(self, method, server_ip, proto_objs, proto_obj):
        res = self.py.exec_rpc(method, server_ip)
        json_obj = json.loads(res)
        for list_i in range(len(json_obj)):
            vproto_obj = pbjson.dict2pb(proto_obj, json_obj[list_i])
            proto_objs.extend([vproto_obj])
        return proto_objs
