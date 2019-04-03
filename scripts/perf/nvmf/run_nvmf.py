#!/usr/bin/env python3

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


class Server:
    def __init__(self, name, username, password, mode, nic_ips, transport):
        self.name = name
        self.mode = mode
        self.username = username
        self.password = password
        self.nic_ips = nic_ips
        self.transport = transport.lower()

        if not re.match("^[A-Za-z0-9]*$", name):
            self.log_print("Please use a name which contains only letters or numbers")
            sys.exit(1)

    def log_print(self, msg):
        print("[%s] %s" % (self.name, msg), flush=True)


class Target(Server):
    def __init__(self, name, username, password, mode, nic_ips, transport="rdma", use_null_block=False, sar_settings=None):
        super(Target, self).__init__(name, username, password, mode, nic_ips, transport)
        self.null_block = bool(use_null_block)
        self.enable_sar = False
        if sar_settings:
            self.enable_sar, self.sar_delay, self.sar_interval, self.sar_count = sar_settings

        self.script_dir = os.path.dirname(os.path.abspath(sys.argv[0]))
        self.spdk_dir = os.path.abspath(os.path.join(self.script_dir, "../../../"))

    def zip_spdk_sources(self, spdk_dir, dest_file):
        self.log_print("Zipping SPDK source directory")
        fh = zipfile.ZipFile(dest_file, "w", zipfile.ZIP_DEFLATED)
        for root, directories, files in os.walk(spdk_dir):
            for file in files:
                fh.write(os.path.relpath(os.path.join(root, file)))
        fh.close()
        self.log_print("Done zipping")

    def read_json_stats(self, file):
        with open(file, "r") as json_data:
            data = json.load(json_data)
            job_pos = 0  # job_post = 0 because using aggregated results

            # Check if latency is in nano or microseconds to choose correct dict key
            def get_lat_unit(key_prefix, dict_section):
                # key prefix - lat, clat or slat.
                # dict section - portion of json containing latency bucket in question
                # Return dict key to access the bucket and unit as string
                for k, v in dict_section.items():
                    if k.startswith(key_prefix):
                        return k, k.split("_")[1]

            read_iops = float(data["jobs"][job_pos]["read"]["iops"])
            read_bw = float(data["jobs"][job_pos]["read"]["bw"])
            lat_key, lat_unit = get_lat_unit("lat", data["jobs"][job_pos]["read"])
            read_avg_lat = float(data["jobs"][job_pos]["read"][lat_key]["mean"])
            read_min_lat = float(data["jobs"][job_pos]["read"][lat_key]["min"])
            read_max_lat = float(data["jobs"][job_pos]["read"][lat_key]["max"])
            clat_key, clat_unit = get_lat_unit("clat", data["jobs"][job_pos]["read"])
            read_p99_lat = float(data["jobs"][job_pos]["read"][clat_key]["percentile"]["99.000000"])

            if "ns" in lat_unit:
                read_avg_lat, read_min_lat, read_max_lat = [x / 1000 for x in [read_avg_lat, read_min_lat, read_max_lat]]
            if "ns" in clat_unit:
                read_p99_lat = read_p99_lat / 1000

            write_iops = float(data["jobs"][job_pos]["write"]["iops"])
            write_bw = float(data["jobs"][job_pos]["write"]["bw"])
            lat_key, lat_unit = get_lat_unit("lat", data["jobs"][job_pos]["write"])
            write_avg_lat = float(data["jobs"][job_pos]["write"][lat_key]["mean"])
            write_min_lat = float(data["jobs"][job_pos]["write"][lat_key]["min"])
            write_max_lat = float(data["jobs"][job_pos]["write"][lat_key]["max"])
            clat_key, clat_unit = get_lat_unit("clat", data["jobs"][job_pos]["write"])
            write_p99_lat = float(data["jobs"][job_pos]["write"][clat_key]["percentile"]["99.000000"])

            if "ns" in lat_unit:
                write_avg_lat, write_min_lat, write_max_lat = [x / 1000 for x in [write_avg_lat, write_min_lat, write_max_lat]]
            if "ns" in clat_unit:
                write_p99_lat = write_p99_lat / 1000

        return [read_iops, read_bw, read_avg_lat, read_min_lat, read_max_lat, read_p99_lat,
                write_iops, write_bw, write_avg_lat, write_min_lat, write_max_lat, write_p99_lat]

    def parse_results(self, results_dir, initiator_count=None, run_num=None):
        files = os.listdir(results_dir)
        fio_files = filter(lambda x: ".fio" in x, files)
        json_files = [x for x in files if ".json" in x]

        # Create empty results file
        csv_file = "nvmf_results.csv"
        with open(os.path.join(results_dir, csv_file), "w") as fh:
            header_line = ",".join(["Name",
                                    "read_iops", "read_bw", "read_avg_lat_us",
                                    "read_min_lat_us", "read_max_lat_us", "read_p99_lat_us",
                                    "write_iops", "write_bw", "write_avg_lat_us",
                                    "write_min_lat_us", "write_max_lat_us", "write_p99_lat_us"])
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

    def measure_sar(self, results_dir, sar_file_name):
        self.log_print("Waiting %d delay before measuring SAR stats" % self.sar_delay)
        time.sleep(self.sar_delay)
        out = subprocess.check_output("sar -P ALL %s %s" % (self.sar_interval, self.sar_count), shell=True).decode(encoding="utf-8")
        with open(os.path.join(results_dir, sar_file_name), "w") as fh:
            for line in out.split("\n"):
                if "Average" in line and "CPU" in line:
                    self.log_print("Summary CPU utilization from SAR:")
                    self.log_print(line)
                if "Average" in line and "all" in line:
                    self.log_print(line)
            fh.write(out)


class Initiator(Server):
    def __init__(self, name, username, password, mode, nic_ips, ip, transport="rdma", nvmecli_dir=None, workspace="/tmp/spdk"):
        super(Initiator, self).__init__(name, username, password, mode, nic_ips, transport)
        self.ip = ip
        self.spdk_dir = workspace

        if nvmecli_dir:
            self.nvmecli_bin = os.path.join(nvmecli_dir, "nvme")
        else:
            self.nvmecli_bin = "nvme"  # Use system-wide nvme-cli

        self.ssh_connection = paramiko.SSHClient()
        self.ssh_connection.set_missing_host_key_policy(paramiko.AutoAddPolicy)
        self.ssh_connection.connect(self.ip, username=self.username, password=self.password)
        self.remote_call("sudo rm -rf %s/nvmf_perf" % self.spdk_dir)
        self.remote_call("mkdir -p %s" % self.spdk_dir)

    def __del__(self):
        self.ssh_connection.close()

    def put_file(self, local, remote_dest):
        ftp = self.ssh_connection.open_sftp()
        ftp.put(local, remote_dest)
        ftp.close()

    def get_file(self, remote, local_dest):
        ftp = self.ssh_connection.open_sftp()
        ftp.get(remote, local_dest)
        ftp.close()

    def remote_call(self, cmd):
        stdin, stdout, stderr = self.ssh_connection.exec_command(cmd)
        out = stdout.read().decode(encoding="utf-8")
        err = stderr.read().decode(encoding="utf-8")
        return out, err

    def copy_result_files(self, dest_dir):
        self.log_print("Copying results")

        if not os.path.exists(dest_dir):
            os.mkdir(dest_dir)

        # Get list of result files from initiator and copy them back to target
        stdout, stderr = self.remote_call("ls %s/nvmf_perf" % self.spdk_dir)
        file_list = stdout.strip().split("\n")

        for file in file_list:
            self.get_file(os.path.join(self.spdk_dir, "nvmf_perf", file),
                          os.path.join(dest_dir, file))
        self.log_print("Done copying results")

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

            stdout, stderr = self.remote_call(nvme_discover_cmd)
            if stdout:
                nvme_discover_output = nvme_discover_output + stdout

        subsystems = re.findall(r'trsvcid:\s(\d+)\s+'  # get svcid number
                                r'subnqn:\s+([a-zA-Z0-9\.\-\:]+)\s+'  # get NQN id
                                r'traddr:\s+(\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3})',  # get IP address
                                nvme_discover_output)  # from nvme discovery output
        subsystems = filter(lambda x: x[-1] in address_list, subsystems)
        subsystems = list(set(subsystems))
        subsystems.sort(key=lambda x: x[1])
        self.log_print("Found matching subsystems on target side:")
        for s in subsystems:
            self.log_print(s)

        return subsystems

    def gen_fio_config(self, rw, rwmixread, block_size, io_depth, subsys_no, num_jobs=None, ramp_time=0, run_time=10):
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
            subsystems = self.discover_subsystems(self.nic_ips, subsys_no)
            bdev_conf = self.gen_spdk_bdev_conf(subsystems)
            self.remote_call("echo '%s' > %s/bdev.conf" % (bdev_conf, self.spdk_dir))
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
        if num_jobs:
            fio_config = fio_config + "numjobs=%s" % num_jobs
        fio_config = fio_config + filename_section

        fio_config_filename = "%s_%s_%s_m_%s" % (block_size, io_depth, rw, rwmixread)
        if hasattr(self, "num_cores"):
            fio_config_filename += "_%sCPU" % self.num_cores
        fio_config_filename += ".fio"

        self.remote_call("mkdir -p %s/nvmf_perf" % self.spdk_dir)
        self.remote_call("echo '%s' > %s/nvmf_perf/%s" % (fio_config, self.spdk_dir, fio_config_filename))
        self.log_print("Created FIO Config:")
        self.log_print(fio_config)

        return os.path.join(self.spdk_dir, "nvmf_perf", fio_config_filename)

    def run_fio(self, fio_config_file, run_num=None):
        job_name, _ = os.path.splitext(fio_config_file)
        self.log_print("Starting FIO run for job: %s" % job_name)
        if run_num:
            for i in range(1, run_num + 1):
                output_filename = job_name + "_run_" + str(i) + "_" + self.name + ".json"
                cmd = "sudo /usr/src/fio/fio %s --output-format=json --output=%s" % (fio_config_file, output_filename)
                output, error = self.remote_call(cmd)
                self.log_print(output)
                self.log_print(error)
        else:
            output_filename = job_name + "_" + self.name + ".json"
            cmd = "sudo /usr/src/fio/fio %s --output-format=json --output=%s" % (fio_config_file, output_filename)
            output, error = self.remote_call(cmd)
            self.log_print(output)
            self.log_print(error)
        self.log_print("FIO run finished. Results in: %s" % output_filename)


class KernelTarget(Target):
    def __init__(self, name, username, password, mode, nic_ips,
                 use_null_block=False, sar_settings=None, transport="rdma", nvmet_dir=None, **kwargs):
        super(KernelTarget, self).__init__(name, username, password, mode, nic_ips,
                                           transport, use_null_block, sar_settings)

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
                "trtype": "%s" % self.transport,
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
                        "trtype": "%s" % self.transport
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
            if len(self.nic_ips) > 1:
                print("Testing with null block limited to single RDMA NIC.")
                print("Please specify only 1 IP address.")
                exit(1)
            self.subsys_no = 1
            self.kernel_tgt_gen_nullblock_conf(self.nic_ips[0])
        else:
            print("Configuring with NVMe drives.")
            nvme_list = get_nvme_devices()
            self.kernel_tgt_gen_subsystem_conf(nvme_list, self.nic_ips)
            self.subsys_no = len(nvme_list)

        nvmet_command(self.nvmet_bin, "clear")
        nvmet_command(self.nvmet_bin, "restore kernel.conf")
        self.log_print("Done configuring kernel NVMeOF Target")


class SPDKTarget(Target):
    def __init__(self, name, username, password, mode, nic_ips, num_cores, num_shared_buffers=4096,
                 use_null_block=False, sar_settings=None, transport="rdma", **kwargs):
        super(SPDKTarget, self).__init__(name, username, password, mode, nic_ips, transport, use_null_block, sar_settings)
        self.num_cores = num_cores
        self.num_shared_buffers = num_shared_buffers

    def spdk_tgt_configure(self):
        self.log_print("Configuring SPDK NVMeOF target via RPC")
        numa_list = get_used_numa_nodes()

        # Create RDMA transport layer
        rpc.nvmf.nvmf_create_transport(self.client, trtype=self.transport, num_shared_buffers=self.num_shared_buffers)
        self.log_print("SPDK NVMeOF transport layer:")
        rpc.client.print_dict(rpc.nvmf.get_nvmf_transports(self.client))

        if self.null_block:
            nvme_section = self.spdk_tgt_add_nullblock()
            subsystems_section = self.spdk_tgt_add_subsystem_conf(self.nic_ips, req_num_disks=1)
        else:
            nvme_section = self.spdk_tgt_add_nvme_conf()
            subsystems_section = self.spdk_tgt_add_subsystem_conf(self.nic_ips)
        self.log_print("Done configuring SPDK NVMeOF Target")

    def spdk_tgt_add_nullblock(self):
        self.log_print("Adding null block bdev to config via RPC")
        rpc.bdev.construct_null_bdev(self.client, 102400, 4096, "Nvme0n1")
        self.log_print("SPDK Bdevs configuration:")
        rpc.client.print_dict(rpc.bdev.get_bdevs(self.client))

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
            rpc.bdev.construct_nvme_bdev(self.client, name="Nvme%s" % i, trtype="PCIe", traddr=bdf)

        self.log_print("SPDK Bdevs configuration:")
        rpc.client.print_dict(rpc.bdev.get_bdevs(self.client))

    def spdk_tgt_add_subsystem_conf(self, ips=None, req_num_disks=None):
        self.log_print("Adding subsystems to config")
        if not req_num_disks:
            req_num_disks = get_nvme_devices_count()

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
                rpc.nvmf.nvmf_subsystem_create(self.client, nqn, serial,
                                               allow_any_host=True, max_namespaces=8)
                rpc.nvmf.nvmf_subsystem_add_ns(self.client, nqn, bdev_name)

                rpc.nvmf.nvmf_subsystem_add_listener(self.client, nqn,
                                                     trtype=self.transport,
                                                     traddr=ip,
                                                     trsvcid="4420",
                                                     adrfam="ipv4")

        self.log_print("SPDK NVMeOF subsystem configuration:")
        rpc.client.print_dict(rpc.nvmf.get_nvmf_subsystems(self.client))

    def tgt_start(self):
        self.subsys_no = get_nvme_devices_count()
        self.log_print("Starting SPDK NVMeOF Target process")
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


class KernelInitiator(Initiator):
    def __init__(self, name, username, password, mode, nic_ips, ip, transport, **kwargs):
        super(KernelInitiator, self).__init__(name, username, password, mode, nic_ips, ip, transport)

    def __del__(self):
        self.ssh_connection.close()

    def kernel_init_connect(self, address_list, subsys_no):
        subsystems = self.discover_subsystems(address_list, subsys_no)
        self.log_print("Below connection attempts may result in error messages, this is expected!")
        for subsystem in subsystems:
            self.log_print("Trying to connect %s %s %s" % subsystem)
            self.remote_call("sudo %s connect -t %s -s %s -n %s -a %s -i 8" % (self.nvmecli_bin, self.transport, *subsystem))
            time.sleep(2)

    def kernel_init_disconnect(self, address_list, subsys_no):
        subsystems = self.discover_subsystems(address_list, subsys_no)
        for subsystem in subsystems:
            self.remote_call("sudo %s disconnect -n %s" % (self.nvmecli_bin, subsystem[1]))
            time.sleep(1)

    def gen_fio_filename_conf(self):
        out, err = self.remote_call("lsblk -o NAME -nlp")
        nvme_list = [x for x in out.split("\n") if "nvme" in x]

        filename_section = ""
        for i, nvme in enumerate(nvme_list):
            filename_section = "\n".join([filename_section,
                                          "[filename%s]" % i,
                                          "filename=%s" % nvme])

        return filename_section


class SPDKInitiator(Initiator):
    def __init__(self, name, username, password, mode, nic_ips, ip, num_cores=None, transport="rdma", **kwargs):
        super(SPDKInitiator, self).__init__(name, username, password, mode, nic_ips, ip, transport)
        if num_cores:
            self.num_cores = num_cores

    def install_spdk(self, local_spdk_zip):
        self.put_file(local_spdk_zip, "/tmp/spdk_drop.zip")
        self.log_print("Copied sources zip from target")
        self.remote_call("unzip -qo /tmp/spdk_drop.zip -d %s" % self.spdk_dir)

        self.log_print("Sources unpacked")
        self.remote_call("cd %s; git submodule update --init; ./configure --with-rdma --with-fio=/usr/src/fio;"
                         "make clean; make -j$(($(nproc)*2))" % self.spdk_dir)

        self.log_print("SPDK built")
        self.remote_call("sudo %s/scripts/setup.sh" % self.spdk_dir)

    def gen_spdk_bdev_conf(self, remote_subsystem_list):
        header = "[Nvme]"
        row_template = """  TransportId "trtype:{transport} adrfam:IPv4 traddr:{ip} trsvcid:{svc} subnqn:{nqn}" Nvme{i}"""

        bdev_rows = [row_template.format(transport=self.transport,
                                         svc=x[0],
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
    spdk_zip_path = "/tmp/spdk.zip"
    target_results_dir = "/tmp/results"

    with open("./scripts/perf/nvmf/config.json", "r") as config:
        data = json.load(config)

    initiators = []
    fio_cases = []

    for k, v in data.items():
        if "target" in k:
            if data[k]["mode"] == "spdk":
                target_obj = SPDKTarget(name=k, **data["general"], **v)
            elif data[k]["mode"] == "kernel":
                target_obj = KernelTarget(name=k, **data["general"], **v)
        elif "initiator" in k:
            if data[k]["mode"] == "spdk":
                init_obj = SPDKInitiator(name=k, **data["general"], **v)
            elif data[k]["mode"] == "kernel":
                init_obj = KernelInitiator(name=k, **data["general"], **v)
            initiators.append(init_obj)
        elif "fio" in k:
            fio_workloads = itertools.product(data[k]["bs"],
                                              data[k]["qd"],
                                              data[k]["rw"])

            fio_run_time = data[k]["run_time"]
            fio_ramp_time = data[k]["ramp_time"]
            fio_rw_mix_read = data[k]["rwmixread"]
            fio_run_num = data[k]["run_num"] if "run_num" in data[k].keys() else None
            fio_num_jobs = data[k]["num_jobs"] if "num_jobs" in data[k].keys() else None
        else:
            continue

    # Copy and install SPDK on remote initiators
    target_obj.zip_spdk_sources(target_obj.spdk_dir, spdk_zip_path)
    threads = []
    for i in initiators:
        if i.mode == "spdk":
            t = threading.Thread(target=i.install_spdk, args=(spdk_zip_path,))
            threads.append(t)
            t.start()
    for t in threads:
        t.join()

    target_obj.tgt_start()

    # Poor mans threading
    # Run FIO tests
    for block_size, io_depth, rw in fio_workloads:
        threads = []
        configs = []
        for i in initiators:
            if i.mode == "kernel":
                i.kernel_init_connect(i.nic_ips, target_obj.subsys_no)

            cfg = i.gen_fio_config(rw, fio_rw_mix_read, block_size, io_depth, target_obj.subsys_no,
                                   fio_num_jobs, fio_ramp_time, fio_run_time)
            configs.append(cfg)

        for i, cfg in zip(initiators, configs):
            t = threading.Thread(target=i.run_fio, args=(cfg, fio_run_num))
            threads.append(t)
        if target_obj.enable_sar:
            sar_file_name = "_".join([str(block_size), str(rw), str(io_depth), "sar"])
            sar_file_name = ".".join([sar_file_name, "txt"])
            t = threading.Thread(target=target_obj.measure_sar, args=(target_results_dir, sar_file_name))
            threads.append(t)

        for t in threads:
            t.start()
        for t in threads:
            t.join()

        for i in initiators:
            if i.mode == "kernel":
                i.kernel_init_disconnect(i.nic_ips, target_obj.subsys_no)
            i.copy_result_files(target_results_dir)

    target_obj.parse_results(target_results_dir)
