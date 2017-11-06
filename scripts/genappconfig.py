import re
import sys
from subprocess import check_call, check_output

def generate_iscsi_conf(backend):
    if backend == "iscsi_nvme" or backend == "iscsi_rxtxqueue":
        output = check_output("lspci -nnn", shell=True)
        bus_numbers = re.findall("([0-9][0-9]:[0-9][0-9].[0-9]) Non-Volatile memory controller", output)
        nvme_ctrl_num = len(bus_numbers)
        all_luns = int(nvme_ctrl_num)
        filedesc = open(backend, 'w+')
        filedesc.write("\n[Global] \n")
        filedesc.write("\n[Rpc] \n")
        filedesc.write(" Enable No \n")
        filedesc.write(" 127.0.0.1 \n")
        filedesc.write("\n[Nvme] \n")
        for i, value in enumerate(bus_numbers):
            filedesc.write('  TransportId "trtype:PCIe traddr:0000:{}" Nvme{} \n'.format(value, i))
        filedesc.write("  RetryCount {} \n".format(nvme_ctrl_num))
        filedesc.write("  Timeout 0 \n")
        filedesc.write("  ActionOnTimeout None \n")
        filedesc.write("  AdminPollRate 100000 \n")
        filedesc.write("  HotplugEnable Yes \n")
        idx = 0
        for i in range(all_luns):
            target_id = idx + 1
            filedesc.write("\n[TargetNode" + str(target_id) + "]\n")
            filedesc.write("  TargetName disk" + str(target_id) + "\n")
            filedesc.write("  Mapping PortalGroup1 InitiatorGroup1\n")
            filedesc.write("  AuthMethod Auto\n")
            filedesc.write("  AuthGroup  AuthGroup1\n")
            filedesc.write("  UseDigest Auto\n")
            filedesc.write("  QueueDepth 128\n")
            filedesc.write("  LUN0 Nvme" + str(idx) + "n1" + "\n")
            idx = idx + 1
        filedesc.close()

    if backend == "iscsi_aiobackend":
        output = check_output(" ls /dev/sd* ", shell=True)
        aio_devices = re.findall("sd[b-z]", output)
        filedesc = open(backend, 'w+')
        filedesc.write("\n[Global] \n")
        filedesc.write("\n[iSCSI] \n")
        filedesc.write('NodeBase "iqn.2016-06.io.spdk" \n')
        filedesc.write('AuthFile /usr/local/etc/spdk/auth.conf" \n')
        filedesc.write('MinConnectionsPerCore 4" \n')
        filedesc.write('MinConnectionIdleInterval 5000" \n')
        filedesc.write('Timeout 30" \n')
        filedesc.write('DiscoveryAuthMethod Auto" \n')
        filedesc.write('DefaultTime2Wait 2" \n')
        filedesc.write('DefaultTime2Retain 60" \n')
        filedesc.write('DefaultTime2Retain 60ImmediateData Yes" \n')
        filedesc.write('ErrorRecoveryLevel 0" \n')
        filedesc.write("\n[Rpc] \n")
        filedesc.write(" Enable No \n")
        filedesc.write(" 127.0.0.1 \n")
        filedesc.write(" \n [PortalGroup1]\n")
        filedesc.write(" \n Portal DA1 192.168.3.11:3260\n")
        filedesc.write(" \n [InitiatorGroup1] \n InitiatorName ALL\n Netmask 192.168.1.0/24\n")
        filedesc.write(" \n [Gpt] \n Disable Yes \n")
        for i, aio_device in enumerate(aio_devices):
            print aio_device
            filedesc.write("\n[AIO] \n")
            filedesc.write("  AIO /dev/{} AIO0\n".format(aio_device, i))
            idx = 0
            target_id = idx + 1
            filedesc.write("\n[TargetNode" + str(target_id) + "]\n")
            filedesc.write("  TargetName disk" + str(target_id) + "\n")
            filedesc.write("  Mapping PortalGroup1 InitiatorGroup1\n")
            filedesc.write("  AuthMethod Auto\n")
            filedesc.write("  AuthGroup  AuthGroup1\n")
            filedesc.write("  UseDigest Auto\n")
            filedesc.write("  QueueDepth 128\n")
            filedesc.write("  LUN0 AIO" + str(idx) + "\n")
            idx = idx + 1
        filedesc.close()

    if backend == "iscsi_malloc":
        all_luns = 2
        idx = 0
        filedesc = open(backend, 'w+')
        filedesc.write("\n[Global] \n")
        filedesc.write("\n[iSCSI] \n")
        filedesc.write('NodeBase "iqn.2016-06.io.spdk"\n')
        filedesc.write('AuthFile /usr/local/etc/spdk/auth.conf"\n')
        filedesc.write("MinConnectionsPerCore 4n\n")
        filedesc.write("MinConnectionIdleInterval 5000\n")
        filedesc.write("Timeout 30 5000\n")
        filedesc.write("DiscoveryAuthMethod Auto\n")
        filedesc.write("DefaultTime2Wait 2\n")
        filedesc.write("DefaultTime2Retain 60\n")
        filedesc.write("\n[PortalGroup1]\n")
        filedesc.write("\n[InitiatorGroup1]\n")
        filedesc.write("InitiatorName ALL\n")
        filedesc.write("Netmask 192.168.3.0/24\n")
        filedesc.write("\n[Malloc] \n")
        filedesc.write("  NumberOfLuns 2\n")
        filedesc.write("  LunSizeInMB 128\n")
        filedesc.write("  BlockSize 512\n")
        for i in range(all_luns):
            target_id = idx + 1
            filedesc.write("\n[TargetNode" + str(target_id) + "]\n")
            filedesc.write("  TargetName disk" + str(target_id) + "\n")
            filedesc.write("  Mapping PortalGroup1 InitiatorGroup1\n")
            filedesc.write("  AuthMethod Auto\n")
            filedesc.write("  AuthGroup  AuthGroup1\n")
            filedesc.write("  UseDigest Auto\n")
            filedesc.write("  QueueDepth 128\n")
            filedesc.write("  LUN0 Malloc" + str(idx) + "\n")
            idx = idx + 1
        filedesc.close()

    if backend == "iscsi_multiconnection":
        output = check_output("lspci -nnn", shell=True)
        bus_numbers = re.findall("([0-9][0-9]:[0-9][0-9].[0-9]) Non-Volatile memory controller", output)
        nvme_ctrl_num = len(bus_numbers)
        all_luns = int(nvme_ctrl_num)
        filedesc = open(backend, 'w+')
        filedesc.write("\n[Nvme] \n")
        for i, value in enumerate(bus_numbers):
            filedesc.write('TransportID "trtype:PCIe traddr:0000:{}" Nvme{}\n'.format(value, i))
        filedesc.write("  RetryCount 128 \n")
        filedesc.write("  Timeout 0 \n")
        filedesc.write("  ActionOnTimeout None \n")
        filedesc.write("  AdminPollRate 100000 \n")
        filedesc.write("  HotplugEnable Yes \n")
        filedesc.write("\n[Split] \n")
        filedesc.write("  Split Nvme0n1 22 1 \n")
        filedesc.write("  Split Nvme1n1 22 1 \n")
        filedesc.write("  Split Nvme2n1 22 1 \n")
        filedesc.write("  Split Nvme3n1 22 1 \n")
        filedesc.write("  Split Nvme4n1 22 1 \n")
        filedesc.write("  Split Nvme5n1 18 1 \n")
        idx = 0
        target_id = 1
        all_nodes = 22
        for i in range(all_luns):
            node = 0
            for i in range(all_nodes):
                filedesc.write("\n[TargetNode" + str(target_id) + "]\n")
                filedesc.write("  TargetName disk" + str(target_id) + "\n")
                filedesc.write("  Mapping PortalGroup1 InitiatorGroup1\n")
                filedesc.write("  AuthMethod Auto\n")
                filedesc.write("  AuthGroup  AuthGroup1\n")
                filedesc.write("  UseDigest Auto\n")
                filedesc.write("  QueueDepth 32\n")
                filedesc.write("  LUN0 Nvme" + str(idx) + "n1p" + str(node) + "\n")
                node = node + 1
                target_id = target_id + 1
            idx = idx + 1
        filedesc.close()
        check_call("sed -i '/\[TargetNode129/,$d' " + filename, shell=True)

def generate_nvmf_tgt_conf(backend):
    if backend == "nvmf_nvme":
        output = check_output("lspci -nnn", shell=True)
        bus_numbers = re.findall("([0-9][0-9]:[0-9][0-9].[0-9]) Non-Volatile memory controller", output)
        nvme_ctrl_num = len(bus_numbers)
        all_luns = int(nvme_ctrl_num)
        idx = 0
        filedesc = open(backend, 'w+')
	filedesc.write("\n[Global] \n")
	filedesc.write("\n[Rpc] \n")
	filedesc.write(" Enable No \n 127.0.0.1 \n ")
	filedesc.write(" \n[Nvmf] \n")
	filedesc.write(" MaxQueuesPerSession 4\n MaxQueueDepth 1024\n MaxIOSize 131072\n AcceptorPollRate 10000\n")
        filedesc.write("\n[Nvme] \n")
        for i, value in enumerate(bus_numbers):
            filedesc.write('TransportId "trtype:PCIe traddr:0000:{}" Nvme{} \n'.format(value, i))
        filedesc.write("RetryCount {} \n".format(all_luns))
        filedesc.write("Timeout 0 \n")
        filedesc.write("ActionOnTimeout None \n")
        filedesc.write("AdminPollRate 100000 \n")
        filedesc.write("HotplugEnable Yes \n")
        for i in range(all_luns):
            target_id = idx + 1
            filedesc.write("\n[Subsystem" + str(target_id) + "]\n")
            filedesc.write("  NQN nqn.2016-06.io.spdk:cnode" + str(target_id) + "\n")
            filedesc.write("  Core 0\n")
            filedesc.write("  AllowAnyHost Yes\n")
            filedesc.write("  Listen RDMA 192.168.3.11:4420\n")
            filedesc.write("  SN SPDK" + str(target_id) + "\n")
            filedesc.write("  Namespace Nvme" + str(idx) + "n1" + "\n")
            idx = idx + 1
        filedesc.close()

    if backend == "nvmf_malloc":
        #default configure malloc with 2 luns
        all_luns = 2
        idx = 0
        filedesc = open(backend, 'w+')
	filedesc.write("\n[Global] \n")
	filedesc.write("\n[Rpc] \n")
	filedesc.write(" Enable No \n 127.0.0.1 \n ")
	filedesc.write("\n[Malloc]")
	filedesc.write("\nNumberOfLuns 2 ")
	filedesc.write("\nLunSizeInMB 64 \n ")
	filedesc.write("\n[Nvmf] \n MaxQueuesPerSession 4\n MaxQueueDepth 1024\n MaxIOSize 131072\n AcceptorPollRate 10000\n")
	filedesc.write("\n[Nvme] \n Timeout 0 \n ActionOnTimeout None\n AdminPollRate 100000\n HotplugEnable No\n")
        for i in range(all_luns):
            target_id = idx + 1
            filedesc.write("\n[Subsystem" + str(target_id) + "]\n")
            filedesc.write("  NQN nqn.2016-06.io.spdk:cnode" + str(target_id) +"\n")
            filedesc.write("  Core 0\n")
            filedesc.write("  AllowAnyHost Yes\n")
            filedesc.write("  Listen RDMA 192.168.3.11:4420\n")
            filedesc.write("  SN SPDK" + str(target_id) + "\n")
            filedesc.write("  Namespace Malloc" + str(idx) + "\n")
            idx = idx + 1
        filedesc.close()

    if backend == "nvmf_nvme_multiconnection":
        #default configure multiconnection with 128 luns
        all_luns = 128
        idx = 0
        filedesc = open(backend, 'w+')
	filedesc.write("\n[Global] \n")
	filedesc.write("\n[Rpc] \n")
	filedesc.write(" Enable No \n 127.0.0.1 \n ")
	filedesc.write("\n[Nvmf] \n MaxQueuesPerSession 4\n MaxQueueDepth 1024\n MaxIOSize 131072\n AcceptorPollRate 10000\n")
	filedesc.write("\n[Nvme] \n Timeout 0 \n ActionOnTimeout None\n AdminPollRate 100000\n HotplugEnable No\n")
        for i in range(all_luns):
            target_id = idx + 1
            filedesc.write("\n[Subsystem" + str(target_id) + "]\n")
            filedesc.write("  NQN nqn.2016-06.io.spdk:cnode" + str(target_id) + "\n")
            filedesc.write("  Core 0\n")
            filedesc.write("  AllowAnyHost Yes\n")
            filedesc.write("  Listen RDMA 192.168.3.11:4420\n")
            filedesc.write("  SN SPDK" + str(target_id) + "\n")
            filedesc.write("  Namespace Malloc" + str(idx) + "\n")
            idx = idx + 1
        filedesc.close()

    if backend == "nvmf_aiobackend":
        filedesc = open(backend, 'w+')
	filedesc.write("\n[Global] \n")
	filedesc.write("\n[Rpc] \n")
	filedesc.write(" Enable No \n 127.0.0.1 \n ")
	filedesc.write("\n[AIO] \n")
        all_luns = 2
        idx = 0
        for i in range(all_luns):
            target_id = idx + 1
            filedesc.write("\n[Subsystem" + str(target_id) + "]\n")
            filedesc.write("  NQN nqn.2016-06.io.spdk:cnode" + str(target_id) + "\n")
	    filedesc.write("  Core 0\n")
            filedesc.write("  AllowAnyHost Yes\n")
            filedesc.write("  Listen RDMA 192.168.3.11:4420\n")
            filedesc.write("  SN SPDK" + str(target_id) + "\n")
            filedesc.write("  Namespace AIO" + str(idx) + "\n")
            idx = idx + 1
        filedesc.close()

if __name__ == "__main__":
    if (len(sys.argv) >= 2):
        backend_name = sys.argv[1]
        backend_name = sys.argv[1]

        if str(backend_name.split('_')[0]) == "iscsi":
            generate_iscsi_conf(backend_name)

        if str(backend_name.split('_')[0]) == "nvmf":
            generate_nvmf_tgt_conf(backend_name)
    else:
        print "usage:"
        print "     " + sys.argv[0] + " <application name> <configure file name> "
        print "Examples:"
        print "     python genappconfig.py nvmf_nvme"
        print "     python genappconfig.py nvmf_malloc.conf "
        print "     python genappconfig.py nvmf_nvme_multiconnection "
        print "     python genappconfig.py nvmf_aiobackend "
        print "     python genappconfig.py iscsi_nvme "
        print "     python genappconfig.py iscsi_aiobackend "
        print "     python genappconfig.py iscsi_malloc "
        print "     python genappconfig.py iscsi_multiconnection "

