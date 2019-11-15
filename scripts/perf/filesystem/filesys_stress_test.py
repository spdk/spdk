#!/usr/bin/env python3
import logging
import os
import re
import sys
import json
import paramiko
import zipfile
import threading
import subprocess
import itertools
import time
import uuid
import rpc
import rpc.client
from common import *

now = int(round(time.time() * 1000))
timestamp = time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(now / 1000))
timestamp = timestamp.replace(' ', '_')
timestamp = timestamp.replace(':', '_')


class Logger(object):
    def __init__(self, log_file_name, log_level, logger_name):
        self.__logger = logging.getLogger(logger_name)
        if not self.__logger.handlers:
            self.__logger.setLevel(log_level)
            file_handler = logging.FileHandler(log_file_name)
            console_handler = logging.StreamHandler()
            formatter = logging.Formatter(
                '[%(asctime)s] - [line:%(lineno)d] - %(levelname)s: %(message)s')
            file_handler.setFormatter(formatter)
            console_handler.setFormatter(formatter)
            self.__logger.addHandler(file_handler)
            self.__logger.addHandler(console_handler)

    def get_log(self):
        return self.__logger


class Server:
    def __init__(self, name, username, password, mode, nic_ips, transport):
        self.name = name
        self.mode = mode
        self.username = username
        self.password = password
        self.nic_ips = nic_ips
        self.transport = transport.lower()
        # if os._exists("/home/filesystem_log/filesys.log"):
        #     proc = subprocess.Popen("rm -rf /home/filesystem_log/filesys.log", shell=True)
        if not re.match("^[A-Za-z0-9]*$", name):
            self.log_print("Please use a name which contains only letters or numbers")
            sys.exit(1)

    def log_print(self, msg, source=None):
        log_name = "filesys_" + timestamp + ".log"
        self.logger = Logger(log_file_name='/home/filesystem_log/'+log_name, log_level=logging.DEBUG,
                             logger_name=log_name).get_log()
        if source:
            msg = json.dumps(msg, indent=2)
            msg = "[%s] %s" % (source, msg)
        self.logger.info("[%s] %s" % (self.name, msg))


class Target(Server):
    def __init__(self, name, username, password, mode, nic_ips, transport="rdma", use_null_block=False,
                 sar_settings=None):
        super(Target, self).__init__(name, username, password, mode, nic_ips, transport)
        self.null_block = bool(use_null_block)
        self.enable_sar = False
        if sar_settings:
            self.enable_sar, self.sar_delay, self.sar_interval, self.sar_count = sar_settings

        self.script_dir = os.path.dirname(os.path.abspath(sys.argv[0]))
        self.spdk_dir = os.path.abspath(os.path.join(self.script_dir, "../../../"))


class Initiator(Server):
    def __init__(self, name, username, password, mode, nic_ips, ip, transport="rdma", nvmecli_dir=None,
                 workspace="/tmp/spdk", driver="bdev"):
        super(Initiator, self).__init__(name, username, password, mode, nic_ips, transport)
        self.ip = ip
        self.spdk_dir = workspace
        self.driver = driver

        if nvmecli_dir:
            self.nvmecli_bin = os.path.join(nvmecli_dir, "nvme")
        else:
            self.nvmecli_bin = "nvme"  # Use system-wide nvme-cli

        self.ssh_connection = paramiko.SSHClient()
        self.ssh_connection.set_missing_host_key_policy(paramiko.AutoAddPolicy())
        self.ssh_connection.connect(self.ip, username=self.username, password=self.password)
        self.remote_call("sudo rm -rf %s/nvmf_perf" % self.spdk_dir)
        self.remote_call("mkdir -p %s" % self.spdk_dir)

    def __del__(self):
        self.ssh_connection.close()

    def remote_call(self, cmd):
        stdin, stdout, stderr = self.ssh_connection.exec_command(cmd)
        out = stdout.read().decode(encoding="utf-8")
        err = stderr.read().decode(encoding="utf-8")
        return out, err

    def discover_subsystems(self, address_list, subsys_no):
        num_nvmes = range(0, subsys_no)
        nvme_discover_output = ""
        for ip, subsys_no in itertools.product(address_list, num_nvmes):
            self.log_print("Trying to discover: %s:%s" % (ip, 4420 + subsys_no))
            nvme_discover_cmd = ["sudo",
                                 "%s" % self.nvmecli_bin,
                                 "discover", "-t %s" % self.transport,
                                 "-s %s" % (4420 + subsys_no),
                                 "-a %s" % ip]
            nvme_discover_cmd = " ".join(nvme_discover_cmd)
            print(nvme_discover_cmd, 555555555555555555)

            stdout, stderr = self.remote_call(nvme_discover_cmd)
            print(stderr,88888888888888888888)
            if stdout:
                nvme_discover_output = nvme_discover_output + stdout
                print(nvme_discover_output,999999999999999999999)

        subsystems = re.findall(r'trsvcid:\s(\d+)\s+'  # get svcid number
                                r'subnqn:\s+([a-zA-Z0-9\.\-\:]+)\s+'  # get NQN id
                                r'traddr:\s+(\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3})',  # get IP address
                                nvme_discover_output)  # from nvme discovery output
        print(address_list, "----------------", subsystems)
        subsystems = filter(lambda x: x[-1] in address_list, subsystems)
        subsystems = list(set(subsystems))
        subsystems.sort(key=lambda x: x[1])
        self.log_print("Found matching subsystems on target side:")
        for s in subsystems:
            self.log_print(s)

        return subsystems


class SPDKTarget(Target):
    def __init__(self, name, username, password, mode, nic_ips, num_cores, num_shared_buffers=4096,
                 use_null_block=False, sar_settings=None, transport="rdma", **kwargs):
        super(SPDKTarget, self).__init__(name, username, password, mode, nic_ips, transport, use_null_block,
                                         sar_settings)
        self.num_cores = num_cores
        self.num_shared_buffers = num_shared_buffers

    def spdk_tgt_configure(self):
        self.log_print("Configuring SPDK NVMeOF target via RPC")

        # Create RDMA transport layer
        rpc.nvmf.nvmf_create_transport(self.client, trtype=self.transport, num_shared_buffers=self.num_shared_buffers)
        self.log_print("SPDK NVMeOF transport layer:")
        self.log_print(rpc.nvmf.get_nvmf_transports(self.client), "rpc.nvmf")

        if self.null_block:
            self.spdk_tgt_add_nullblock()
            self.spdk_tgt_add_subsystem_conf(self.nic_ips, req_num_disks=1)
        else:
            self.spdk_tgt_add_nvme_conf(4)
            self.spdk_tgt_add_subsystem_conf(self.nic_ips, req_num_disks=4)
        self.log_print("Done configuring SPDK NVMeOF Target")

    def spdk_tgt_add_nullblock(self):
        self.log_print("Adding null block bdev to config via RPC")
        # rpc.bdev.construct_null_bdev(self.client, 102400, 4096, "Nvme0n1")
        rpc.bdev.bdev_null_create(self.client, 102400, 4096, "Nvme0n1")
        self.log_print("SPDK Bdevs configuration:")
        # rpc.client.print_dict(rpc.bdev.get_bdevs(self.client))
        self.log_print(rpc.bdev.bdev_get_bdevs(self.client), "rpc.bdev")

    def spdk_tgt_add_nvme_conf(self, req_num_disks=None):
        self.log_print("Adding NVMe bdevs to config via RPC")
        bdfs = get_nvme_devices_bdf()
        bdfs = [b.replace(":", ".") for b in bdfs]

        if req_num_disks:
            if req_num_disks > len(bdfs):
                self.log_print("ERROR: Requested number of disks is more than available %s" % len(bdfs))
                sys.exit(1)
            else:
                bdfs = bdfs[0:req_num_disks]

        for i, bdf in enumerate(bdfs):
            # rpc.bdev.construct_nvme_bdev(self.client, name="Nvme%s" % i, trtype="PCIe", traddr=bdf)
            rpc.bdev.bdev_nvme_attach_controller(self.client, name="Nvme%s" % i, trtype="PCIe", traddr=bdf)

        self.log_print("SPDK Bdevs configuration:")
        self.log_print(rpc.bdev.bdev_get_bdevs(self.client), "rpc.bdev")

    def spdk_tgt_add_subsystem_conf(self, ips=None, req_num_disks=None):
        self.log_print("Adding subsystems to config")
        if not req_num_disks:
            req_num_disks = get_nvme_devices_count()
        else:
            req_num_disks = req_num_disks

        # Distribute bdevs between provided NICs
        num_disks = range(1, req_num_disks + 1)
        disks_per_ip = int(len(num_disks) / len(ips))
        disk_chunks = [num_disks[i * disks_per_ip:disks_per_ip + disks_per_ip * i] for i in range(0, len(ips))]

        # Create subsystems, add bdevs to namespaces, add listeners
        for ip, chunk in zip(ips, disk_chunks):
            for c in chunk:
                nqn = "nqn.2018-09.io.spdk:cnode%s" % c
                serial = "SPDK00%s" % c
                bdev_name = "Nvme%sn1" % (c - 1)
                # rpc.nvmf.nvmf_subsystem_create(self.client, nqn, serial,
                #                                allow_any_host=True, max_namespaces=8)
                rpc.nvmf.nvmf_create_subsystem(self.client, nqn, serial,
                                               allow_any_host=True, max_namespaces=8)
                rpc.nvmf.nvmf_subsystem_add_ns(self.client, nqn, bdev_name)

                rpc.nvmf.nvmf_subsystem_add_listener(self.client, nqn,
                                                     trtype=self.transport,
                                                     traddr=ip,
                                                     trsvcid=str(4420),
                                                     adrfam="ipv4")

        self.log_print("SPDK NVMeOF subsystem configuration:")
        # rpc.client.print_dict(rpc.nvmf.get_nvmf_subsystems(self.client))
        self.log_print(rpc.nvmf.nvmf_get_subsystems(self.client), "rpc.nvmf")

    def tgt_start(self):
        self.subsys_no = get_nvme_devices_count()
        self.log_print("--------------Starting SPDK NVMeOF Target process--------------")
        #nvmf_app_path = os.path.join(self.spdk_dir, "app/nvmf_tgt/nvmf_tgt &")
        nvmf_app_path = os.path.join(self.spdk_dir, "app/nvmf_tgt/nvmf_tgt")
        command = " ".join([nvmf_app_path, "-m", self.num_cores])
        proc = subprocess.Popen(command, shell=True)
        self.pid = os.path.join(self.spdk_dir, "nvmf.pid")

        with open(self.pid, "w") as fh:
            fh.write(str(proc.pid))
        self.nvmf_proc = proc
        self.log_print("SPDK NVMeOF Target PID=%s" % self.pid)
        self.log_print("Waiting for spdk to initilize...")
        while True:
            if os.path.exists("/var/tmp/spdk.sock"):
                break
            time.sleep(1)
        self.client = rpc.client.JSONRPCClient("/var/tmp/spdk.sock")

        self.spdk_tgt_configure()

    def __del__(self):
        if hasattr(self, "nvmf_proc"):
            try:
                self.nvmf_proc.terminate()
                self.nvmf_proc.wait()
            except Exception as e:
                self.log_print(e)
                self.nvmf_proc.kill()
                self.nvmf_proc.communicate()


class SPDKInitiator(Initiator):
    def __init__(self, name, username, password, mode, nic_ips, ip, num_cores=None, transport="rdma", driver="bdev",
                 **kwargs):
        super(SPDKInitiator, self).__init__(name, username, password, mode, nic_ips, ip, transport, driver=driver)
        if num_cores:
            self.num_cores = num_cores
        self.driver = driver
        self.remote_call("rm -rf /home/nvmfpart0*")

    def discover_connect(self, address_list, subsys_no, n, initiators_count):
        subsystems = self.discover_subsystems(address_list, subsys_no)
        self.log_print("Below connection attempts may result in error messages, this is expected!")
        harf = len(subsystems) / initiators_count
        print(subsys_no, initiators_count, harf,66666666666666666666)
        self.nqn_code = []
        print((n-1)*harf, n*harf, 77777777777777777)
        for subsystem in subsystems[int((n-1)*harf):int(n*harf)]:
            self.log_print("Trying to connect %s %s %s" % subsystem)
            self.remote_call("sudo %s connect -t %s -s %s -n %s -a %s -i 8" %
                             (self.nvmecli_bin, self.transport, subsystem[0], subsystem[1], subsystem[2]))
            self.nqn_code.append(subsystem[1])
            time.sleep(5)
        self.filesys_init("xfs")

    def disconnect(self):
        self.log_print("disconnect the nvme bdev")
        self.log_print("nqn: %s" % self.nqn_code)
        for nqn in self.nqn_code:
            self.remote_call("sudo {0} disconnect -n {1}".format(self.nvmecli_bin, nqn))
            time.sleep(2)

    def filesys_init(self, filesystemtype):
        self.filetype = filesystemtype
        self.kernel_package_path = "/home/storage/linux-4.19.72.tar.gz"
        self.log_print("The kernel package's path is %s" % self.kernel_package_path)
        std, err = self.remote_call("ls {}".format(self.kernel_package_path))
        err_msg = "ls: cannot access '%s': No such file or directory" % self.kernel_package_path
        if err_msg in err:
            self.log_print("Please add kernel source.")
            time.sleep(300)
        output, _ = self.remote_call("lsblk -l -o NAME")
        devices = re.findall("(nvme[0-9]+n1)\n", output)
        # self.device_paths = ['/dev/nvme0n1','/dev/nvme1n1','/dev/nvme2n1',...]
        self.device_paths = ['/dev/' + dev for dev in devices]

    def filesys_parted(self):
        self.log_print("the file system for the devices is: %s" % self.filetype)
        self.new_dev_paths = []
        for dev_path in self.device_paths:
            self.log_print(dev_path)
            # dev_paths = ['/dev/nvme0n1p','/dev/nvme1n1p','/dev/nvme2n1p']
            dev_paths = (dev_path) + "p"
            cmd = "parted -s {} mklabel gpt".format(dev_path)
            print("parted cmd is : ", cmd)
            self.remote_call(cmd)
            # dev : nvme0n1p, nvme1n1p, nvme2n1p
            dev = (dev_path).lstrip('/dev/')
            optimal_io_size, _ = self.remote_call("cat /sys/block/{}/queue/optimal_io_size".format(dev))
            alignment_offset, _ = self.remote_call("cat /sys/block/{}/alignment_offset".format(dev))
            physical_block_size, _ = self.remote_call("cat /sys/block/{}/queue/physical_block_size".format(dev))

            optimal_io_size = int(optimal_io_size)
            alignment_offset = int(alignment_offset)
            physical_block_size = int(physical_block_size)
            sector_num = (optimal_io_size + alignment_offset) / physical_block_size
            if sector_num == 0:
                sector_num = 2048
            sector_number = str(sector_num) + "s"

            cmd = 'parted -s {0} mkpart primary {1} 100% '.format(dev_path, sector_number)
            self.remote_call(cmd)
            new_dev_path = (dev_paths) + "1"
            self.remote_call("mkfs.{0} -f {1}".format(self.filetype, new_dev_path))
            # self.new_dev_paths =  ['/dev/nvme0n1p1','/dev/nvme1n1p1','/dev/nvme2n1p1']
            self.new_dev_paths.append(new_dev_path)
            std, _ = self.remote_call("lsblk")
            self.log_print(std)

    def filesys_mount(self):
        if len(self.new_dev_paths) < 4:
            number = len(self.new_dev_paths)
        else:
            number = 4
        tamp_list = []
        thread_list = []
        for i in range(number):
            dir_name = "/home/nvmfpart0" + str(i) + "/"
            self.remote_call("mkdir -p {}".format(dir_name))
            time.sleep(10)
            cmd = "mount {} {}".format(self.new_dev_paths[i], dir_name)
            self.log_print("----------------[mount] start mount cmd: %s -----------------" % cmd)
            std, err = self.remote_call("mount {} {}".format(self.new_dev_paths[i], dir_name))
            if err:
                self.log_print("----------------[mount] mount filed : %s -----------------" % err)
                for x in range(i):
                    um_dir = "/home/nvmfpart0" + str(x) + "/"
                    self.remote_call("umount {}".format(um_dir))
                self.disconnect()
                os._exit(1)
            self.log_print("----------------[mount] mount done -----------------")
            tamp_list.append(dir_name + "linux-4.19.72")
            self.remote_call("dd if={} of={}/linux-4.19.72.tar.gz".format(self.kernel_package_path, dir_name))
            #self.remote_call("cd {} && tar -zxvf linux-4.19.72.tar.gz".format(dir_name))
            t = threading.Thread(target=self.remote_call, args=("cd {} && tar -xvf linux-4.19.72.tar.gz".format(dir_name),))
            thread_list.append(t)
            t.start()
        for t in thread_list:
            t.join()
        print(tamp_list,3333333333333333333)
        while True:
            tamp_set = set()
            for tamp in tamp_list:
                std, err = self.remote_call("ls {}".format(tamp))
                #err_msg = "ls: cannot access '%s': No such file or directory" % tamp
                err_msg = "No such file or directory"
                if err_msg in err:
                    self.log_print("wait to tar %s" % tamp)
                    time.sleep(30)
                else:
                    tamp_set.add(tamp)
            if len(tamp_set) == len(tamp_list):
                self.log_print("----------------all package tar done -----------------")
                break

    def filesys_kernel_compile(self, compile_count):
        retval = ""
        cmd_list = []
        has_err = False
        std, _ = self.remote_call("nvme list")
        conn_nvme_list = re.findall("(/dev/nvme[0-9]+n1)", std)
        for n in range(len(conn_nvme_list)):
            cmd = "cd /home/nvmfpart0%s" % n + "/linux-4.19.72 && make clean && make -j 64 &"
            cmd_list.append(cmd)
        for i in range(compile_count):
            self.log_print("----------------[kernelcompile] start the %s times-----------------" % (i+1))
            for cmd in cmd_list:
                std, err = self.remote_call(cmd)
                #self.log_print(std)
                if not std or err:
                    self.log_print("----------------compile error is :-----------------")
                    self.log_print(err)
                    self.log_print("---------------------------------")
                    has_err = True
                    retval = "make command failed"
                    break
                index = 0
                for line in std:
                    if "failed" in line:
                        if index < 4:
                            self.log_print(line[index])
                            self.log_print(line[index + 1])
                            self.log_print(line[index + 2])
                            self.log_print(line[index + 3])
                        else:
                            self.log_print(line[index - 3])
                            self.log_print(line[index - 2])
                            self.log_print(line[index - 1])
                            self.log_print(line[index])
                            self.log_print(line[index + 1])
                            self.log_print(line[index + 2])
                            self.log_print(line[index + 3])
                        has_err = True
                        #self.filesys_umount(conn_nvme_list)
                        #self.disconnect()
                time.sleep(20)
            self.log_print("----------------[kernelcompile] end the %s times-----------------" % (i+1))

            if has_err:
                break
            else:
                retval = "All tests passed"
        print(conn_nvme_list,4444444444444444444)
        self.filesys_umount(conn_nvme_list)
        self.disconnect()
        return retval

    def filesys_umount(self, conn_nvme_list):
        flag = True
        for n in range(len(conn_nvme_list)):
            cmd = "umount /home/nvmfpart0%s" % n
            self.log_print(cmd)
            std, err = self.remote_call(cmd)
            if err:
                flag = False
                self.log_print("fail to %s" % cmd)
                break
        if not flag:
            for n in self.new_dev_paths:
                cmd = "umount %s" % n
                self.log_print(cmd)
                self.remote_call(cmd)
        self.log_print("umount done")

    def filesys_test(self, compile_count):
        self.filesys_parted()
        self.filesys_mount()
        retval = self.filesys_kernel_compile(compile_count)
        self.log_print("stress test result: %s" % retval)


def get_config():
    global config_file_path
    if (len(sys.argv) > 1):
        config_file_path = sys.argv[1]
    else:
        script_full_dir = os.path.dirname(os.path.realpath(__file__))
        config_file_path = os.path.join(script_full_dir, "config.json")
    print("Using config file: %s" % config_file_path)
    return config_file_path


if __name__ == '__main__':
    print(1111111111111111111)
    config_file_path = get_config()

    with open(config_file_path, "r") as config:
        data = json.load(config)

    initiators = []

    for k, v in data.items():
        if "target" in k:
            target_obj = SPDKTarget(name=k, **data["general"], **v)

        elif "initiator" in k:
            init_obj = SPDKInitiator(name=k, **data["general"], **v)
            initiators.append(init_obj)
        else:
            continue

    target_obj.tgt_start()

    threads = []
    print(target_obj.subsys_no,666666666666666666666)
    for i in initiators:
        n = 1
        t = threading.Thread(target=i.discover_connect, args=(i.nic_ips, target_obj.subsys_no, n, len(initiators)))
        threads.append(t)
        t.start()
        n += 1
    for t in threads:
        t.join()

    filesys_threads = []
    for i in initiators:
        t = threading.Thread(target=i.filesys_test, args=(1,))
        filesys_threads.append(t)
        t.start()
    for t in threads:
        t.join()
    #time.sleep(100000)
