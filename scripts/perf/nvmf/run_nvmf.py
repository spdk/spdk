#!/usr/bin/env python3

import os
import re
import sys
import json
import paramiko
import zipfile
import threading
import subprocess
from common import *
import itertools
import time
import uuid
# sys.path.append(os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(sys.argv[0])), "../../")))
# import rpc
# print(rpc)


class Server:
    def __init__(self, name, username, password, mode, rdma_ips):
        self.name = name
        self.mode = mode
        self.username = username
        self.password = password
        self.rdma_ips = rdma_ips

        if not re.match("^[A-Za-z0-9]*$", name):
            self.log_print("Please use a name which contains only letters or numbers")
            sys.exit(1)

    def log_print(self, msg):
        print("[%s] %s" % (self.name, msg), flush=True)


class Target(Server):
    def __init__(self, name, username, password, mode, rdma_ips, use_null_block=False, measure_sar=False):
        super(Target, self).__init__(name, username, password, mode, rdma_ips)
        self.null_block = bool(use_null_block)
        self.sar = measure_sar

        self.script_dir = os.path.dirname(os.path.abspath(sys.argv[0]))
        self.spdk_dir = os.path.abspath(os.path.join(self.script_dir, "../../../"))

    def zip_spdk_sources(self, spdk_dir, dest_file):
        self.log_print("Zipping SPDK source")
        fh = zipfile.ZipFile(dest_file, 'w', zipfile.ZIP_DEFLATED)
        for root, dirs, files in os.walk(spdk_dir):
            for file in files:
                fh.write(os.path.relpath(os.path.join(root, file)))
        fh.close()
        self.log_print("Done zipping")

    def read_json_stats(self, file):
        with open(os.path.join(file, file), "r") as json_data:
            data = json.load(json_data)
            job_pos = 0  # job_post = 0 because using aggregated results
            lat = "lat_ns"

            read_iops = float(data["jobs"][job_pos]["read"]["iops"])
            read_bw = float(data["jobs"][job_pos]["read"]["bw"])
            read_avg_lat = float(data["jobs"][job_pos]["read"][lat]["mean"])
            read_min_lat = float(data["jobs"][job_pos]["read"][lat]["min"])
            read_max_lat = float(data["jobs"][job_pos]["read"][lat]["max"])
            write_iops = float(data["jobs"][job_pos]["write"]["iops"])
            write_bw = float(data["jobs"][job_pos]["write"]["bw"])
            write_avg_lat = float(data["jobs"][job_pos]["write"][lat]["mean"])
            write_min_lat = float(data["jobs"][job_pos]["write"][lat]["min"])
            write_max_lat = float(data["jobs"][job_pos]["write"][lat]["max"])

        return [read_iops, read_bw, read_avg_lat, read_min_lat, read_max_lat,
                write_iops, write_bw, write_avg_lat, write_min_lat, write_max_lat]

    def parse_results(self, results_dir, initiator_count=None, run_num=None):
        files = os.listdir(results_dir)
        fio_files = filter(lambda x: ".fio" in x, files)
        json_files = list(filter(lambda x: ".json" in x, files))

        # Create empty results file
        csv_file = "nvmf_results.csv"
        with open(os.path.join(results_dir, csv_file), "w") as fh:
            header_line = ",".join(["Name",
                                    "read_iops", "read_bw", "read_avg_lat", "read_min_lat", "read_max_lat",
                                    "write_iops", "write_bw", "write_avg_lat", "write_min_lat", "write_max_lat"])
            fh.write(header_line + "\n")
        rows = set()

        for fio_config in fio_files:
            self.log_print("Getting FIO stats for %s" % fio_config)
            job_name, _ = os.path.splitext(fio_config)

            # If "_CPU" exists in name - ignore it
            # Initiators for the same job could have diffrent num_cores parameter
            job_name = re.sub(r"_\d+CPU", "", job_name)
            job_result_files = [x for x in json_files if job_name in x]
            self.log_print("Matching result files for current fio config:")
            for j in job_result_files:
                self.log_print("\t %s" % j)

            # There may have been more than 1 initiator used in test, need to check that
            # Result files are created so that string after last "_" separator is server name
            inits_names = set([os.path.splitext(x)[0].split("_")[-1] for x in job_result_files])
            inits_avg_results = []
            for i in inits_names:
                self.log_print("\tGetting stats for initiator %s" % i)
                # There may have been more than 1 test run for this job, calculate average results for initiator
                i_results = [x for x in job_result_files if i in x]

                separate_stats = []
                for r in i_results:
                    stats = self.read_json_stats(os.path.join(results_dir, r))
                    separate_stats.append(stats)
                    self.log_print(stats)

                z = [sum(c) for c in zip(*separate_stats)]
                z = [c/len(separate_stats) for c in z]
                inits_avg_results.append(z)

                self.log_print("\tAverage results for initiator %s" % i)
                self.log_print(z)

            # Sum average results of all initiators running this FIO job
            self.log_print("\tTotal results for %s from all initiators" % fio_config)
            for a in inits_avg_results:
                self.log_print(a)
            total = ["{0:.3f}".format(sum(c)) for c in zip(*inits_avg_results)]
            rows.add(",".join([job_name, *total]))

            # Save results to file
            for row in rows:
                with open(os.path.join(results_dir, csv_file), "a") as fh:
                    fh.write(row + "\n")


class Initiator(Server):
    def __init__(self, name, username, password, mode, rdma_ips, ip, workspace="/tmp/spdk"):
        super(Initiator, self).__init__(name, username, password, mode, rdma_ips)
        self.ip = ip
        self.spdk_dir = workspace

        self.ssh_connection = paramiko.SSHClient()
        self.ssh_connection.set_missing_host_key_policy(paramiko.AutoAddPolicy)
        self.ssh_connection.connect(self.ip, username=self.username, password=self.password)
        stdin, stdout, stderr = self.ssh_connection.exec_command("sudo rm -rf %s/nvmf_perf" % self.spdk_dir)
        stdout.channel.recv_exit_status()
        stdin, stdout, stderr = self.ssh_connection.exec_command("mkdir -p %s" % self.spdk_dir)
        stdout.channel.recv_exit_status()

    def __del__(self):
        self.ssh_connection.close()

    def copy_file(self, local, remote_dest):
        ftp = self.ssh_connection.open_sftp()
        ftp.put(local, remote_dest)
        ftp.close()

    def get_file(self, remote, local_dest):
        ftp = self.ssh_connection.open_sftp()
        ftp.get(remote, local_dest)
        ftp.close()

    def copy_result_files(self, dest_dir):
        self.log_print("Copying results")

        if not os.path.exists(dest_dir):
            os.mkdir(dest_dir)

        stdin, stdout, stderr = self.ssh_connection.exec_command("ls %s/nvmf_perf" % self.spdk_dir)

        file_list = stdout.read().decode(encoding="utf-8").strip().split("\n")
        for file in file_list:
            self.get_file(os.path.join(self.spdk_dir, "nvmf_perf", file),
                          os.path.join(dest_dir, file))
        self.log_print("Done copying results")

    def discover_subsystems(self, address_list, subsys_no):
        num_nvmes = range(0, subsys_no)
        nvme_discover_output = ""
        for ip, subsys_no in itertools.product(address_list, num_nvmes):
            self.log_print("Trying to discover: %s:%s" % (ip, 4420 + subsys_no))
            nvme_discover_cmd = ["sudo", "nvme", "discover", "-t rdma", "-s %s" % (4420 + subsys_no), "-a %s" % ip]
            nvme_discover_cmd = " ".join(nvme_discover_cmd)

            print(nvme_discover_cmd)
            stdin, stdout, stderr = self.ssh_connection.exec_command(nvme_discover_cmd)
            out = stdout.read().decode(encoding="utf-8")
            if out:
                nvme_discover_output = nvme_discover_output + out

        subsystems = re.findall(r'trsvcid:\s(\d+)\s+'  # get svcid number
                                r'subnqn:\s+([a-zA-Z0-9\.\-\:]+)\s+'  # get NQN id
                                r'traddr:\s+(\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3})',  # get IP address
                                nvme_discover_output)  # from nvme discovery output
        subsystems = filter(lambda x: x[-1] in address_list, subsystems)
        subsystems = list(set(subsystems))

        return subsystems

    def gen_fio_config(self, rw, rwmixread, block_size, io_depth, subsys_no, ramp_time=0, run_time=10):
        fio_conf_template = """
[global]
ioengine={ioengine}
{spdk_conf}
thread=1
group_reporting=1
direct=1

norandommap=1
rw={rw}
rwmixread={rwmixread}
bs={block_size}
iodepth={io_depth}
time_based=1
ramp_time={ramp_time}
runtime={run_time}
"""
        if "spdk" in self.mode:
            subsystems = self.discover_subsystems(self.rdma_ips, subsys_no)
            bdev_conf = self.gen_spdk_bdev_conf(subsystems)
            stdin, stdout, stderr = self.ssh_connection.exec_command("echo '%s' > %s/bdev.conf" % (bdev_conf, self.spdk_dir))
            # print(stdout.read(), stderr.read())
            ioengine = "%s/examples/bdev/fio_plugin/fio_plugin" % self.spdk_dir
            spdk_conf = "spdk_conf=%s/bdev.conf" % self.spdk_dir
            filename_section = self.gen_fio_filename_conf(subsystems)
        else:
            ioengine = "libaio"
            spdk_conf = ""
            filename_section = self.gen_fio_filename_conf()

        fio_config = fio_conf_template.format(ioengine=ioengine, spdk_conf=spdk_conf,
                                              rw=rw, rwmixread=rwmixread, block_size=block_size,
                                              io_depth=io_depth, ramp_time=ramp_time, run_time=run_time)
        fio_config = fio_config + filename_section

        fio_config_filename = "%s_%s_%s_m_%s" % (block_size, io_depth, rw, rwmixread)
        if hasattr(self, "num_cores"):
            fio_config_filename += "_%sCPU" % self.num_cores
        fio_config_filename += ".fio"

        stdin, stdout, stderr = self.ssh_connection.exec_command("mkdir -p %s/nvmf_perf" % self.spdk_dir)
        stdin, stdout, stderr = self.ssh_connection.exec_command(
            "echo '%s' > %s/nvmf_perf/%s" % (fio_config, self.spdk_dir, fio_config_filename))
        stdout.channel.recv_exit_status()

        return os.path.join(self.spdk_dir, "nvmf_perf", fio_config_filename)

    def run_fio(self, fio_config_file, run_num=None):
        job_name, _ = os.path.splitext(fio_config_file)
        self.log_print("Starting FIO run for job: %s" % job_name)
        if run_num:
            for i in range(1, run_num + 1):
                output_filename = job_name + "_run_" + str(i) + "_" + self.name + ".json"
                cmd = "sudo /usr/src/fio/fio %s --output-format=json --output=%s" % (fio_config_file, output_filename)
                stdin, stdout, stderr = self.ssh_connection.exec_command(cmd)
                output = stdout.read().decode(encoding="utf-8")
                error = stderr.read().decode(encoding="utf-8")
                self.log_print(output)
                self.log_print(error)
        else:
            output_filename = job_name + "_" + self.name + ".json"
            cmd = "sudo /usr/src/fio/fio %s --output-format=json --output=%s" % (fio_config_file, output_filename)
            stdin, stdout, stderr = self.ssh_connection.exec_command(cmd)
            output = stdout.read().decode(encoding="utf-8")
            error = stderr.read().decode(encoding="utf-8")
            self.log_print(output)
            self.log_print(error)
        self.log_print("FIO run finished. Results in: %s" % output_filename)


class KernelTarget(Target):
    def __init__(self, name, username, password, mode, rdma_ips, use_null_block=False, nvmet_dir=None, **kwargs):
        super(KernelTarget, self).__init__(name, username, password, mode, rdma_ips, use_null_block)

        if nvmet_dir:
            self.nvmet_bin = os.path.join(nvmet_dir, "nvmetcli")
        else:
            self.nvmet_bin = "nvmetcli"

    def __del__(self):
        nvmet_command(self.nvmet_bin, "clear")

    def kernel_tgt_gen_nullblock_conf(self, address):
        nvmet_cfg = {
            "ports": [],
            "hosts": [],
            "subsystems": [],
        }

        nvmet_cfg["subsystems"].append({
            "allowed_hosts": [],
            "attr": {
                "allow_any_host": "1",
                "version": "1.3"
            },
            "namespaces": [
                {
                    "device": {
                        "path": "/dev/nullb0",
                        "uuid": "%s" % uuid.uuid4()
                    },
                    "enable": 1,
                    "nsid": 1
                }
            ],
            "nqn": "nqn.2018-09.io.spdk:cnode1"
        })

        nvmet_cfg["ports"].append({
            "addr": {
                "adrfam": "ipv4",
                "traddr": address,
                "trsvcid": "4420",
                "trtype": "rdma"
            },
            "portid": 1,
            "referrals": [],
            "subsystems": ["nqn.2018-09.io.spdk:cnode1"]
        })
        with open("kernel.conf", 'w') as fh:
            fh.write(json.dumps(nvmet_cfg, indent=2))

    def kernel_tgt_gen_subsystem_conf(self, nvme_list, address_list):

        nvmet_cfg = {
            "ports": [],
            "hosts": [],
            "subsystems": [],
        }

        # Split disks between NIC IP's
        disks_per_ip = int(len(nvme_list) / len(address_list))
        disk_chunks = [nvme_list[i * disks_per_ip:disks_per_ip + disks_per_ip * i] for i in range(0, len(address_list))]

        subsys_no = 1
        port_no = 0
        for ip, chunk in zip(address_list, disk_chunks):
            for disk in chunk:
                nvmet_cfg["subsystems"].append({
                    "allowed_hosts": [],
                    "attr": {
                        "allow_any_host": "1",
                        "version": "1.3"
                    },
                    "namespaces": [
                        {
                            "device": {
                                "path": disk,
                                "uuid": "%s" % uuid.uuid4()
                            },
                            "enable": 1,
                            "nsid": subsys_no
                        }
                    ],
                    "nqn": "nqn.2018-09.io.spdk:cnode%s" % subsys_no
                })

                nvmet_cfg["ports"].append({
                    "addr": {
                        "adrfam": "ipv4",
                        "traddr": ip,
                        "trsvcid": "%s" % (4420 + port_no),
                        "trtype": "rdma"
                    },
                    "portid": subsys_no,
                    "referrals": [],
                    "subsystems": ["nqn.2018-09.io.spdk:cnode%s" % subsys_no]
                })
                subsys_no += 1
                port_no += 1

        with open("kernel.conf", "w") as fh:
            fh.write(json.dumps(nvmet_cfg, indent=2))
        pass

    def tgt_start(self):
        self.log_print("Configuring kernel NVMeOF Target")

        if self.null_block:
            print("Configuring with null block device.")
            if len(self.rdma_ips) > 1:
                print("Testing with null block limited to single RDMA NIC.")
                print("Please specify only 1 IP address.")
                exit(1)
            self.subsys_no = 1
            self.kernel_tgt_gen_nullblock_conf(self.rdma_ips[0])
        else:
            print("Configuring with NVMe drives.")
            nvme_list = get_nvme_devices()
            self.kernel_tgt_gen_subsystem_conf(nvme_list, self.rdma_ips)
            self.subsys_no = len(nvme_list)

        nvmet_command(self.nvmet_bin, "clear")
        nvmet_command(self.nvmet_bin, "restore kernel.conf")
        self.log_print("Done configuring kernel NVMeOF Target")


class SPDKTarget(Target):
    def __init__(self, name, username, password, mode, rdma_ips, num_cores, use_null_block=False, **kwargs):
        super(SPDKTarget, self).__init__(name, username, password, mode, rdma_ips, use_null_block)
        self.num_cores = int(num_cores)

    def gen_tgt_conf(self):
        self.log_print("Generating NVMeOF configuration file")
        numa_list = get_used_numa_nodes()
        core_mask = gen_core_mask(self.num_cores, numa_list)

        with open(os.path.join(self.script_dir, "spdk_tgt_template.conf"), "r") as fh:
            global_section = fh.read()
            global_section = global_section.format(core_mask=core_mask)

        if self.null_block:
            nvme_section = self.spdk_tgt_add_nullblock()
            subsystems_section = self.spdk_tgt_add_subsystem_conf(self.rdma_ips, req_num_disks=1)
        else:
            nvme_section = self.spdk_tgt_add_nvme_conf()
            subsystems_section = self.spdk_tgt_add_subsystem_conf(self.rdma_ips)

        with open(os.path.join(self.spdk_dir, "spdk_tgt.conf"), "w") as config_fh:
            config_fh.write(global_section)
            config_fh.write(nvme_section)
            config_fh.write(subsystems_section)

    def spdk_tgt_add_nullblock(self):
        self.log_print("Adding null block bdev to config")
        null_section = """[Null]
      Dev Nvme0n1 102400 4096"""

        return null_section

    def spdk_tgt_add_nvme_conf(self, req_num_disks=None):
        self.log_print("Adding NVMe to config")
        header = """[Nvme]
      RetryCount 4
      Timeout 0
      ActionOnTimeout None
      AdminPollRate 100000
      HotplugEnable No
      """

        row_template = """  TransportId "trtype:PCIe traddr:{pci_addr}" Nvme{i}"""

        bdfs = get_nvme_devices_bdf()
        bdfs = [b.replace(":", ".") for b in bdfs]

        if req_num_disks:
            if req_num_disks > len(bdfs):
                self.log_print("ERROR: Requested number of disks is more than available %s" % len(bdfs))
                sys.exit(1)
            else:
                bdfs = bdfs[0:req_num_disks]

        nvme_section = [row_template.format(pci_addr=b, i=i) for i, b in enumerate(bdfs)]
        # join header and all NVMe entries into one string
        nvme_section = ("\n".join([header, "\n".join(nvme_section), "\n"]))

        self.log_print("Done adding NVMe to config")
        return str(nvme_section)

    def spdk_tgt_add_subsystem_conf(self, ips=None, req_num_disks=None):
        self.log_print("Adding subsystems to config")
        if not req_num_disks:
            req_num_disks = get_nvme_devices_count()

        subsystem_template = """[Subsystem{sys_no}]
      NQN nqn.2018-09.io.spdk:cnode{sys_no}
      Listen RDMA {ip}:4420
      AllowAnyHost Yes
      SN SPDK000{sys_no}
      Namespace Nvme{nvme_no}n1 1
        """
        # Split disks between NIC IP's
        num_disks = range(1, req_num_disks + 1)
        disks_per_ip = int(len(num_disks) / len(ips))
        disk_chunks = [num_disks[i * disks_per_ip:disks_per_ip + disks_per_ip * i] for i in range(0, len(ips))]

        subsystems_section = ""
        for ip, chunk in zip(ips, disk_chunks):
            subsystems_chunk = [subsystem_template.format(sys_no=x,
                                                          nvme_no=x - 1,
                                                          ip=ip) for x in chunk]
            subsystems_chunk = "\n".join(subsystems_chunk)
            subsystems_section = "\n".join([subsystems_section, subsystems_chunk])
        return str(subsystems_section)

    def tgt_start(self):
        nvmf_app_path = os.path.join(self.spdk_dir, "app/nvmf_tgt/nvmf_tgt")
        nvmf_cfg_file = os.path.join(self.spdk_dir, "spdk_tgt.conf")

        self.gen_tgt_conf()
        self.subsys_no = get_nvme_devices_count()

        self.log_print("Starting SPDK NVMeOF Target process")
        command = [nvmf_app_path, "-c", nvmf_cfg_file]
        command = " ".join(command)
        proc = subprocess.Popen(command, shell=True)
        self.pid = os.path.join(self.spdk_dir, "nvmf.pid")

        with open(self.pid, "w") as fh:
            fh.write(str(proc.pid))
        self.nvmf_proc = proc
        self.log_print("SPDK NVMeOF Target PID=%s" % self.pid)

    def __del__(self):
        if hasattr(self, "nvmf_proc"):
            try:
                self.nvmf_proc.terminate()
                self.nvmf_proc.wait()
            except Exception as e:
                self.log_print(e)


class KernelInitiator(Initiator):
    def __init__(self, name, username, password, mode, rdma_ips, ip, **kwargs):
        super(KernelInitiator, self).__init__(name, username, password, mode, rdma_ips, ip)

    def __del__(self):
        self.ssh_connection.close()

    def kernel_init_connect(self, address_list, subsys_no):
        subsystems = self.discover_subsystems(address_list, subsys_no)
        self.log_print("Below connection attempts may result in error messages, this is expected!")
        for subsystem in subsystems:
            self.log_print("Trying to connect %s %s %s" % subsystem)
            cmd = "sudo nvme connect -t rdma -s %s -n %s -a %s" % subsystem
            stdin, stdout, stderr = self.ssh_connection.exec_command(cmd)
            time.sleep(1)

    def kernel_init_disconnect(self, address_list, subsys_no):
        subsystems = self.discover_subsystems(address_list, subsys_no)
        for subsystem in subsystems:
            cmd = "sudo nvme disconnect -n %s" % subsystem[1]
            stdin, stdout, stderr = self.ssh_connection.exec_command(cmd)
            time.sleep(1)

    def gen_fio_filename_conf(self):
        stdin, stdout, stderr = self.ssh_connection.exec_command("lsblk -o NAME -nlp")
        out = stdout.read().decode(encoding="utf-8")
        nvme_list = [x for x in out.split("\n") if "nvme" in x]

        filename_section = ""
        for i, nvme in enumerate(nvme_list):
            filename_section = "\n".join([filename_section,
                                          "[filename%s]" % i,
                                          "filename=%s" % nvme])

        return filename_section


class SPDKInitiator(Initiator):
    def __init__(self, name, username, password, mode, rdma_ips, ip, num_cores=None, **kwargs):
        super(SPDKInitiator, self).__init__(name, username, password, mode, rdma_ips, ip)
        if num_cores:
            self.num_cores = num_cores

    def install_spdk(self, local_spdk_zip):
        self.copy_file(local_spdk_zip, "/tmp/spdk_drop.zip")
        self.log_print("Copied sources zip from target")
        stdin, stdout, stderr = self.ssh_connection.exec_command("unzip -qo /tmp/spdk_drop.zip -d %s" % self.spdk_dir)
        stdout.channel.recv_exit_status()

        self.log_print("Sources unpacked")
        stdin, stdout, stderr = self.ssh_connection.exec_command(
            "cd %s; git submodule update --init; ./configure --with-rdma --with-fio=/usr/src/fio;"
            "make clean; make -j$(($(nproc)*2))" % self.spdk_dir)
        stdout.channel.recv_exit_status()

        self.log_print("SPDK built")
        stdin, stdout, stderr = self.ssh_connection.exec_command("sudo %s/scripts/setup.sh" % self.spdk_dir)
        stdout.channel.recv_exit_status()

    def gen_spdk_bdev_conf(self, remote_subsystem_list):
        header = "[Nvme]"
        row_template = """  TransportId "trtype:RDMA adrfam:IPv4 traddr:{ip} trsvcid:{svc} subnqn:{nqn}" Nvme{i}"""

        bdev_rows = [row_template.format(svc=x[0],
                                         nqn=x[1],
                                         ip=x[2],
                                         i=i) for i, x in enumerate(remote_subsystem_list)]
        bdev_rows = "\n".join(bdev_rows)
        bdev_section = "\n".join([header, bdev_rows])
        return bdev_section

    def gen_fio_filename_conf(self, remote_subsystem_list):
        subsystems = [str(x) for x in range(0, len(remote_subsystem_list))]

        # If num_cpus exists then limit FIO to this number of CPUs
        # Otherwise - each connected subsystem gets its own CPU
        if hasattr(self, 'num_cores'):
            self.log_print("Limiting FIO workload execution to %s cores" % self.num_cores)
            threads = range(0, int(self.num_cores))
        else:
            threads = range(0, len(subsystems))

        n = int(len(subsystems) / len(threads))

        filename_section = ""
        for t in threads:
            header = "[filename%s]" % t
            disks = "\n".join(["filename=Nvme%sn1" % x for x in subsystems[n * t:n + n * t]])
            filename_section = "\n".join([filename_section, header, disks])

        return filename_section


if __name__ == "__main__":
    with open("./scripts/perf/nvmf/config.json", "r") as config:
        data = json.load(config)

    initiators = []
    fio_cases = []
    for k, v in data.items():
        if "target" in k:
            if data[k]["mode"] == "spdk":
                target_obj = SPDKTarget(name=k, username=uname, password=passwd, **v)
            elif data[k]["mode"] == "kernel":
                target_obj = KernelTarget(name=k, username=uname, password=passwd, **v)
        elif "initiator" in k:
            if data[k]["mode"] == "spdk":
                init_obj = SPDKInitiator(name=k, username=uname, password=passwd, **v)
            elif data[k]["mode"] == "kernel":
                init_obj = KernelInitiator(name=k, username=uname, password=passwd, **v)
            initiators.append(init_obj)
        elif "general" in k:
            uname = data[k]["username"]
            passwd = data[k]["password"]
            continue
        elif "fio" in k:
            fio_workloads = itertools.product(data[k]["bs"],
                                              data[k]["qd"],
                                              data[k]["rw"])

            fio_run_time = data[k]["run_time"]
            fio_ramp_time = data[k]["ramp_time"]
            fio_rw_mix_read = data[k]["rwmixread"]
            if "run_num" in data[k].keys():
                fio_run_num = data[k]["run_num"]
            else:
                fio_run_num = None
        else:
            continue

    target_obj.zip_spdk_sources(target_obj.spdk_dir, "/tmp/spdk.zip")
    threads = []
    test = "/tmp/spdk.zip"
    for i in initiators:
        if i.mode == "spdk":
            t = threading.Thread(target=i.install_spdk, args=(test,))
            threads.append(t)
            t.start()
    for t in threads:
        t.join()

    target_obj.tgt_start()
    while True:
        if os.path.exists("/var/tmp/spdk.sock"):
            break
        time.sleep(1)

    # Poor mans threading
    for block_size, io_depth, rw in fio_workloads:
        threads = []
        configs = []
        for i in initiators:
            if i.mode == "kernel":
                i.kernel_init_connect(init_obj.rdma_ips, target_obj.subsys_no)
            cfg = i.gen_fio_config(rw, fio_rw_mix_read, block_size, io_depth,
                                   target_obj.subsys_no, fio_ramp_time, fio_run_time)
            configs.append(cfg)
        for i, cfg in zip(initiators, configs):
            t = threading.Thread(target=i.run_fio, args=(cfg, fio_run_num))
            threads.append(t)
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        for i in initiators:
            if i.mode == "kernel":
                i.kernel_init_disconnect(init_obj.rdma_ips, target_obj.subsys_no)

    for i in initiators:
        i.copy_result_files("/tmp/results")

    target_obj.parse_results("/tmp/results")
