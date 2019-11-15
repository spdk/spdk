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
import logging

now = int(round(time.time() * 1000))
timestamp = time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(now / 1000))
timestamp = timestamp.replace(' ', '_')
timestamp = timestamp.replace(':', '_')
result_file = '/home/iscsi_log/reserve_result_' + timestamp + '.txt'
result_dict = {}

class Logger(object):
    def __init__(self, log_file_name, log_level, logger_name):
        self.__logger = logging.getLogger(logger_name)
        if not self.__logger.handlers:
            self.__logger.setLevel(log_level)
            file_handler = logging.FileHandler(log_file_name)
            console_handler = logging.StreamHandler()
            formatter = logging.Formatter(
                '[%(asctime)s] - %(levelname)s: %(message)s')
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
        # if os.path.exists("/home/iscsi_log/run_iscsi.log"):
        #     proc = subprocess.Popen("rm -rf /home/iscsi_log/run_iscsi.log", shell=True)
        if not re.match("^[A-Za-z0-9]*$", name):
            self.log_print("Please use a name which contains only letters or numbers")
            sys.exit(1)

    def log_print(self, msg, source=None):
        log_name = 'run_iscsi_' + timestamp + '.log'
        self.logger = Logger(log_file_name='/home/iscsi_log/'+log_name, log_level=logging.DEBUG, logger_name=log_name).get_log()
        if source:
            msg = json.dumps(msg, indent=2)
            msg = "[%s] %s" % (source, msg)
        self.logger.info("[%s] %s" % (self.name, msg))
        

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
        for root, directories, files in os.walk(spdk_dir, followlinks=True):
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
        time_stamp = results_dir.split('/')[-1]
        csv_file = "nvmf_results_" + time_stamp + ".csv"

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
        self.log_print("You can find the test results in the file %s" % os.path.join(results_dir, csv_file))

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
    def __init__(self, name, username, password, mode, nic_ips, ip, transport="rdma", nvmecli_dir=None, workspace="/tmp/spdk", driver="bdev"):
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
            # nvme_discover_cmd = ["sudo",
            #                      "%s" % self.nvmecli_bin,
            #                      "discover", "-t %s" % self.transport,
            #                      "-s %s" % (4420 + subsys_no),
            #                      "-a %s" % ip]
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
        print(address_list,"----------------",subsystems)
        subsystems = filter(lambda x: x[-1] in address_list, subsystems)
        subsystems = list(set(subsystems))
        subsystems.sort(key=lambda x: x[1])
        self.log_print("Found matching subsystems on target side:")
        for s in subsystems:
            self.log_print(s)

        return subsystems

    def iscsi_login(self):
        iscsi_logout_delete = "iscsiadm -m node --logout && iscsiadm -m node -o delete"
        self.remote_call(iscsi_logout_delete)
        # iscsiadm -m discovery -t sendtargets -p 192.168.89.11:3260
        iscsi_discover_cmd = "iscsiadm -m discovery -t sendtargets -p %s:3260" % self.nic_ips[0]
        stdout, stderr = self.remote_call(iscsi_discover_cmd)
        # iscsiadm -m node --login -p 192.168.89.11:3260
        iscsi_login_cmd = "iscsiadm -m node --login -p %s:3260" % self.nic_ips[0]
        stdout, stderr = self.remote_call(iscsi_login_cmd)
        if "successful" in stdout:
            self.log_print("iscsi login ok")
        else:
            self.log_print("iscsi login failed")
        self.log_print("iscsi login done")
        # self.remote_call(iscsi_logout_delete)


    def gen_fio_config(self, rw, rwmixread, block_size, io_depth, subsys_no, num_jobs=None, ramp_time=0, run_time=10,
                       fio_mem_size_mb=None, fio_size=None):
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
            if fio_size:
                bdev_conf, bdev_rows = self.gen_spdk_bdev_conf(subsystems, fio_size)
            else:
                bdev_conf, bdev_rows = self.gen_spdk_bdev_conf(subsystems)
            self.remote_call("echo '%s' > %s/bdev.conf" % (bdev_conf, self.spdk_dir))
            ioengine = "%s/examples/" % self.spdk_dir + "%s/fio_plugin/fio_plugin" % self.driver
            spdk_conf = "spdk_conf=%s/bdev.conf" % self.spdk_dir
            if bdev_rows:
                filename_section = self.gen_fio_filename_conf(subsystems, bdev_rows)
            else:
                filename_section = self.gen_fio_filename_conf(subsystems)
        else:
            ioengine = "libaio"
            spdk_conf = ""
            filename_section = self.gen_fio_filename_conf()
        if self.driver == "nvme":
            fio_conf_template = fio_conf_template.replace("{spdk_conf}", '')
        fio_config = fio_conf_template.format(ioengine=ioengine, spdk_conf=spdk_conf,
                                              rw=rw, rwmixread=rwmixread, block_size=block_size,
                                              io_depth=io_depth, ramp_time=ramp_time, run_time=run_time)
        if num_jobs:
            fio_config = fio_config + "numjobs=%s" % num_jobs
        if fio_mem_size_mb:
            fio_config = fio_config + "\n" + "mem_size_mb=%s" % fio_mem_size_mb
        if fio_size:
            fio_config = fio_config + "\n" + "size=%s" % fio_size
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

    def iscsi_create_fio_config(self, size, q_depth, devices, test, run_time, num_jobs, verify):
        fio_template_bak = """
        [global]
        thread=1
        invalidate=1
        rw=%(testtype)s
        time_based=1
        runtime=%(runtime)s
        ioengine=libaio
        direct=1
        bs=%(blocksize)d
        iodepth=%(iodepth)d
        norandommap=%(norandommap)d
        numjobs=%(numjobs)d
        %(verify)s
        verify_dump=1

        """
        
        fio_template = """
        [global]
        thread=1
        invalidate=1
        rw={testtype}
        time_based=1
        runtime={runtime}
        ioengine=libaio
        direct=1
        bs={blocksize}
        iodepth={iodepth}
        norandommap={norandommap}
        numjobs={numjobs}
        {verify}
        verify_dump=1
        """

        verify_template = """
        do_verify=1
        verify=crc32c-intel
        """
        fio_job_template = """
        [job%(jobnumber)d]
        filename=%(device)s

        """
        norandommap = 0
        if not verify:
            verifyfio = ""
            norandommap = 1
        else:
            verifyfio = verify_template
        print(type(num_jobs), 33333333333333333333333)
        """fiofile = fio_template % {"blocksize": size, "iodepth": q_depth,
                                  "testtype": test, "runtime": run_time,
                                  "norandommap": norandommap, "verify": verifyfio,
                                  "numjobs": num_jobs}"""
        fiofile = fio_template.format(blocksize=size, iodepth=q_depth, testtype=test, runtime=run_time, norandommap=norandommap, verify=verifyfio, numjobs=num_jobs)
        for (i, dev) in enumerate(devices):
            fiofile += fio_job_template % {"jobnumber": i, "device": dev}
        fio_config_filename = "%s_%s_%s_m_%s" % (size, q_depth, test, 100)
        if hasattr(self, "num_cores"):
            fio_config_filename += "_%sCPU" % self.num_cores
        fio_config_filename += ".fio"
        self.remote_call("mkdir -p %s/nvmf_perf" % self.spdk_dir)
        std,err = self.remote_call("echo '%s' > %s/nvmf_perf/%s" % (fiofile, self.spdk_dir, fio_config_filename))
        print(std, "-------------------------",err)
        self.log_print("Created FIO Config:")
        self.log_print(fiofile)
        return os.path.join(self.spdk_dir, "nvmf_perf", fio_config_filename)

    def iscsi_set_device_parameter(self, devices, filename_template, value):
        valid_value = True

        for dev in devices:
            filename = filename_template % dev
            self.log_print(filename)
            file_opt_cmd = "echo %s > %s" % (value, filename)
            self.log_print(file_opt_cmd)
            stdout, stderr = self.remote_call(file_opt_cmd)
            self.log_print(stdout)
            if stderr:
                self.log_print(stderr)
                valid_value = False
                continue
        if valid_value:
            self.log_print("write file done")
        return valid_value

    def get_device_path(self):
        stdout, _ = self.remote_call("iscsiadm -m session -P 3")
        devices = re.findall("Attached scsi disk (sd[a-z]+)", stdout)
        device_paths = ['/dev/' + dev for dev in devices]
        self.log_print("Device paths:")
        self.log_print(device_paths)
        try:
            stdout, stderr = self.remote_call("which sg_persist")
            fio_executable = stdout.split()[0]
        except subprocess.CalledProcessError as e:
            sys.stderr.write(str(e))
            sys.stderr.write("\nCan't find the sg_persist binary, please install it.\n")
            sys.exit(1)
        return device_paths

    def reserve_register_key(self, value, device_paths):
        #  sg_persist --out --register --param-sark=123abc /dev/sdb
        results = False
        for device in device_paths:
            register_cmd = "sg_persist --out --register --param-sark=%s %s" % (value, device)
            stdout, stderr = self.remote_call(register_cmd)
            self.log_print(register_cmd)
            self.log_print(stdout)
            if stderr:
                self.log_print("[%s] register failed, the value: [%s]" % (device, value))
                self.log_print(stderr)
                results = False
                break
            else:
                self.log_print("[%s] register ok, the value: [%s]" % (device, value))
                results = True
                continue
        return results

    def reserve_read_keys(self, device_paths):
        # sg_persist --in --read-keys --device=/dev/sdb
        results = False
        keys_list = []
        for device in device_paths:
            read_cmd = "sg_persist --in --read-keys --device=%s" % device
            stdout, stderr = self.remote_call(read_cmd)
            self.log_print(read_cmd)
            self.log_print(stdout)
            if stderr:
                self.log_print("[%s] read keys failed" % device)
                self.log_print(stderr)
                results = False
                break
            else:
                get_keys = re.findall(r"0x(.*)$", stdout)
                keys_list += get_keys
                self.log_print("[%s] read keys ok : %s" % (device, get_keys))
                results = True
                continue
        return results, keys_list

    def reserve_register_ignore(self, value, device_paths):
        # sg_persist --out --register-ignore --param-sark=abc123 /dev/sdb
        results = False
        for device in device_paths:
            register_cmd = "sg_persist --out --register-ignore --param-sark=%s %s" % (value, device)
            stdout, stderr = self.remote_call(register_cmd)
            self.log_print(register_cmd)
            self.log_print(stdout)
            if stderr:
                self.log_print("[%s] register ignore failed, the new value: %s" % (device, value))
                self.log_print(stderr)
                results = False
                break
            else:
                self.log_print("[%s] register ignore ok, the new value: %s" % (device, value))
                results = True
                continue
        return results

    def reserve_prout_type(self, type_num, reg_value, device_paths):
        # sg_persist --out --reserve --prout-type=5 --param-rk=123abc /dev/sdb
        results = False
        for device in device_paths:
            reserve_cmd = "sg_persist --out --reserve --prout-type=%d --param-rk=%s %s" % (type_num, reg_value, device)
            stdout, stderr = self.remote_call(reserve_cmd)
            self.log_print(reserve_cmd)
            self.log_print(stdout)
            if stderr:
                self.log_print("[%s] reserve failed, prout-type=%d, [register can write, and only one PRs owner]" % (device, type_num))
                self.log_print(stderr)
                results = False
                break
            else:
                self.log_print("[%s] reserve ok, prout-type=%d, [register can write, and only one PRs owner]" % (device, type_num))
                results = True
                continue
        return results

    def reserve_clear_prout_type(self, type_num, reg_value, device_paths):
        # sg_persist --out --clear --prout-type=5 --param-rk=456abc /dev/sdb
        results = False
        for device in device_paths:
            clear_cmd = "sg_persist --out --clear --prout-type=%d --param-rk=%s %s" % (type_num, reg_value, device)
            stdout, stderr = self.remote_call(clear_cmd)
            self.log_print(clear_cmd)
            self.log_print(stdout)
            if stderr:
                self.log_print("[%s] reserve clear failed, prout-type=%d, [register can write, and only one PRs owner]" % (
                device, type_num))
                self.log_print(stderr)
                results = False
                break
            else:
                self.log_print(
                    "[%s] reserve clear ok, prout-type=%d, [register can write, and only one PRs owner]" % (device, type_num))
                results = True
                continue
        return results

    def reserve_release_prout_type(self, type_num, reg_value, device_paths):
        # sg_persist --out --release --prout-type=5 --param-rk=456abc /dev/sdb
        results = False
        for device in device_paths:
            release_cmd = "sg_persist --out --release --prout-type=%d --param-rk=%s %s" % (type_num, reg_value, device)
            stdout, stderr = self.remote_call(release_cmd)
            self.log_print(release_cmd)
            self.log_print(stdout)
            if stderr:
                self.log_print(stderr)
                if stderr == "PR out (Release): bad field in cdb or parameter list (perhaps unsupported service action)\n":
                    self.log_print(
                        "[%s] reserve release ok, but reservation not release still.prout-type=%d, [register can write, and only one PRs owner]" % (
                            device, type_num))
                    results = True
                else:
                    self.log_print(
                        "[%s] reserve release failed, prout-type=%d, [register can write, and only one PRs owner]" % (
                            device, type_num))
                    results = False
                    break
            else:
                self.log_print(
                    "[%s] reserve release ok, prout-type=%d, [register can write, and only one PRs owner]" % (
                    device, type_num))
                results = True
                continue
        return results

    def reserve_query_prout_type(self, device_paths):
        # sg_persist --in -k  --device=/dev/sdb
        results = False
        for device in device_paths:
            query_cmd = "sg_persist --in -k  --device=%s" % (device)
            stdout, stderr = self.remote_call(query_cmd)
            self.log_print(query_cmd)
            self.log_print(stdout)
            if stderr:
                self.log_print(stderr)
                self.log_print("[%s] reserve query the clear keys failed" % (device))
                results = False
                break
            else:
                if "there are NO registered reservation keys" not in stdout:
                    self.log_print("[%s] reserve query the clear keys failed" % (device))
                    results = False
                    break
                else:
                    self.log_print("[%s] reserve query the clear keys ok,and there are NO registered reservation keys" % (device))
                    results = True
                    continue
        return results

    def reserve_preempt_prout_type(self, type_num, new_reg_value, old_reg_value, device_paths):
        # sg_persist --out --preempt --prout-type=5 --param-rk=456abc --param-sark=123abc /dev/sdb
        results = False
        for device in device_paths:
            preempt_cmd = "sg_persist --out --preempt --prout-type=%d --param-rk=%s --param-sark=%s %s" % (type_num, new_reg_value, old_reg_value, device)
            stdout, stderr = self.remote_call(preempt_cmd)
            self.log_print(preempt_cmd)
            self.log_print(stdout)
            if stderr:
                self.log_print(stderr)
                if "PR out (Preempt): Reservation conflict" in stderr:
                    self.log_print(
                        "[%s] reserve Preempt failed, param-rk=%s[new value] --param-sark=%s[old value]" % (
                            device, new_reg_value, old_reg_value))
                results = False
                break
            else:
                self.log_print(
                    "[%s] reserve Preempt ok, param-rk=%s[new value] --param-sark=%s[old value]" % (
                        device, new_reg_value, old_reg_value))
                results = True
                continue
        return results

    def iscsi_configure_devices(self, devices):
        for dev in devices:
            retry = 30
            file_path = "/sys/block/%s/queue/nomerges" % dev
            while retry > 0:
                stdout, stderr = self.remote_call("ls {}".format(file_path))
                if "ls: cannot access '%s': No such file or directory" % file_path in stderr:
                    retry = retry - 1
                    time.sleep(0.1)
                else:
                    break
            
        self.iscsi_set_device_parameter(devices, "/sys/block/%s/queue/nomerges", "2")
        self.iscsi_set_device_parameter(devices, "/sys/block/%s/queue/nr_requests", "128")
        requested_qd = 128
        qd = requested_qd
        while qd > 0:
            valid_value = self.iscsi_set_device_parameter(devices, "/sys/block/%s/device/queue_depth", str(qd))
            if valid_value:
                break
            else:
                qd = qd - 1
        if qd == 0:
            print("Could not set block device queue depths.")
        elif qd < requested_qd:
            print("Requested queue_depth {} but only {} is supported.".format(str(requested_qd), str(qd)))
        if not self.iscsi_set_device_parameter(devices, "/sys/block/%s/queue/scheduler", "noop"):
            self.iscsi_set_device_parameter(devices, "/sys/block/%s/queue/scheduler", "none")

    def iscsi_fio_config(self, io_size, queue_depth, test_type, runtime, num_jobs, verify):
        # get_iscsi_target_devices
        stdout, _ = self.remote_call("iscsiadm -m session -P 3")
        devices = re.findall("Attached scsi disk (sd[a-z]+)", stdout)
        self.iscsi_configure_devices(devices)
        try:
            stdout, stderr = self.remote_call("which fio")
            fio_executable = stdout.split()[0]
        except subprocess.CalledProcessError as e:
            sys.stderr.write(str(e))
            sys.stderr.write("\nCan't find the fio binary, please install it.\n")
            sys.exit(1)
        device_paths = ['/dev/' + dev for dev in devices]
        self.log_print("Device paths:")
        self.log_print(device_paths)
        fio_config = self.iscsi_create_fio_config(io_size, queue_depth, device_paths, test_type, runtime, num_jobs, verify)
        return fio_config

    def run_fio(self, fio_config_file, run_num=None):
        result = True
        job_name, _ = os.path.splitext(fio_config_file)
        self.log_print("Starting FIO run for job: %s" % job_name)
        if run_num:
            for i in range(1, run_num + 1):
                output_filename = job_name + "_run_" + str(i) + "_" + self.name + ".json"
                cmd = "sudo /usr/src/fio/fio %s --output-format=json --output=%s" % (fio_config_file, output_filename)
                output, error = self.remote_call(cmd)
                self.log_print(output)
                self.log_print("11111111111111111111111111111")
                if error:
                    self.log_print(error)
                    result = False
        else:
            output_filename = job_name + "_" + self.name + ".json"
            cmd = "sudo /usr/src/fio/fio %s --output-format=json --output=%s" % (fio_config_file, output_filename)
            output, error = self.remote_call(cmd)
            self.log_print(output)
            self.log_print("11111111111111111111111111111")
            if error:
                self.log_print(error)
                result = False
        self.log_print("FIO run finished. Results in: %s" % output_filename)
        return result

class SPDKTarget(Target):
    def __init__(self, name, username, password, mode, nic_ips, num_cores, num_shared_buffers=4096,
                 use_null_block=False, sar_settings=None, transport="rdma", **kwargs):
        super(SPDKTarget, self).__init__(name, username, password, mode, nic_ips, transport, use_null_block, sar_settings)
        self.num_cores = num_cores
        self.num_shared_buffers = num_shared_buffers

    def spdk_tgt_configure(self, initiator_num):
        self.log_print("Configuring SPDK NVMeOF target via RPC")
        numa_list = get_used_numa_nodes()

        # Create target group
        rpc.iscsi.iscsi_create_portal_group(self.client, [{'host': self.nic_ips[0], 'port': "3260"}], 1)
        # Create target group
        net_mask = self.nic_ips[0].split('.')
        net_mask[-1] = "0/24"
        net_mask = (".").join(net_mask)
        for i in range(initiator_num):
            rpc.iscsi.iscsi_create_initiator_group(self.client, (i+1), ["Any"], [net_mask])
        self.log_print("Display current portal group configuration:")
        # rpc.client.print_dict(rpc.iscsi.iscsi_get_portal_groups(self.client))
        self.log_print(rpc.iscsi.iscsi_get_portal_groups(self.client), "rpc.iscsi")
        self.log_print("Display current initiator group configuration:")
        # rpc.client.print_dict(rpc.iscsi.iscsi_get_initiator_groups(self.client))
        self.log_print(rpc.iscsi.iscsi_get_initiator_groups(self.client), "rpc.iscsi")
        if self.null_block:
            nvme_section = self.spdk_tgt_add_nullblock()
            subsystems_section = self.spdk_tgt_add_subsystem_conf(self.nic_ips, req_num_disks=1)
        else:
            nvme_section = self.spdk_tgt_add_nvme_conf(1)
        self.log_print("Done configuring Target Node")

    def spdk_tgt_add_nullblock(self):
        self.log_print("Adding null block bdev to config via RPC")
        # rpc.bdev.construct_null_bdev(self.client, 102400, 4096, "Nvme0n1")
        rpc.bdev.bdev_null_create(self.client, 102400, 4096, "Nvme0n1")
        self.log_print("SPDK Bdevs configuration:")
        # rpc.client.print_dict(rpc.bdev.get_bdevs(self.client))
        # rpc.client.print_dict(rpc.bdev.bdev_get_bdevs(self.client))
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

        luns_list = []
        for i, bdf in enumerate(bdfs):
            # rpc.bdev.construct_nvme_bdev(self.client, name="Nvme%s" % i, trtype="PCIe", traddr=bdf)
            rpc.bdev.bdev_nvme_attach_controller(self.client, name="Nvme%s" % i, trtype="PCIe", traddr=bdf)
            bdev_dict = {"bdev_name": "Nvme%s" % i+"n1", "lun_id": 0}
            luns_list.append(bdev_dict)
        tgt_name = "Target" + str(len(luns_list))
        tgt_alias = tgt_name + "_alias"
        rpc.iscsi.iscsi_create_target_node(self.client, luns_list, [{"pg_tag": 1, "ig_tag": 1}], tgt_name, tgt_alias, 64, disable_chap=True)
        self.log_print("SPDK Bdevs configuration:")
        # rpc.client.print_dict(rpc.bdev.bdev_get_bdevs(self.client))
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
                                                     trsvcid=str(4420 + c-1),
                                                     adrfam="ipv4")

        self.log_print("SPDK NVMeOF subsystem configuration:")
        # rpc.client.print_dict(rpc.nvmf.get_nvmf_subsystems(self.client))
        # rpc.client.print_dict(rpc.nvmf.nvmf_get_subsystems(self.client))
        self.log_print(rpc.nvmf.nvmf_get_subsystems(self.client), "rpc.nvmf")
    def tgt_start(self, initiator_num):
        self.subsys_no = get_nvme_devices_count()
        self.log_print("Starting SPDK NVMeOF Target process")
        nvmf_app_path = os.path.join(self.spdk_dir, "app/iscsi_tgt/iscsi_tgt")
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

        self.spdk_tgt_configure(initiator_num)

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
    def __init__(self, name, username, password, mode, nic_ips, ip, num_cores=None, transport="rdma", driver="bdev",**kwargs):
        super(SPDKInitiator, self).__init__(name, username, password, mode, nic_ips, ip, transport, driver=driver)
        if num_cores:
            self.num_cores = num_cores
        self.driver = driver

    def install_spdk(self, local_spdk_zip):
        self.put_file(local_spdk_zip, "/tmp/spdk_drop.zip")
        self.log_print("Copied sources zip from target")
        self.remote_call("unzip -qo /tmp/spdk_drop.zip -d %s" % self.spdk_dir)

        self.log_print("Sources unpacked")
        self.remote_call("cd %s; git submodule update --init; ./configure --with-rdma --with-fio=/usr/src/fio;"
                         "make clean; make -j$(($(nproc)*2))" % self.spdk_dir)

        self.log_print("SPDK built")
        self.remote_call("sudo %s/scripts/setup.sh" % self.spdk_dir)

    def gen_spdk_bdev_conf(self, remote_subsystem_list, size="8G"):
        if self.driver == "bdev":
            row_template = """  TransportId "trtype:{transport} adrfam:IPv4 traddr:{ip} trsvcid:{svc} subnqn:{nqn}" Nvme{i}"""
            bdev_rows = [row_template.format(transport=self.transport,
                                             svc=x[0],
                                             nqn=x[1],
                                             ip=x[2],
                                             i=i) for i, x in enumerate(remote_subsystem_list)]
        else:
            row_template = """  filename=trtype={transport} adrfam=IPv4 traddr={ip} trsvcid={svc} ns={i}"""
            # tamp_list = [row_template,"size={n}"]
            # row_template = "\n".join(tamp_list)
            bdev_rows = [row_template.format(transport=self.transport,
                                             svc=x[0],
                                             ip=x[2],
                                             i=1) for x in remote_subsystem_list]
        tamp_rows_list = bdev_rows
        header = "[Nvme]"
        bdev_rows = "\n".join(bdev_rows)
        bdev_section = "\n".join([header, bdev_rows])
        if self.driver == "nvme":
            return bdev_section, tamp_rows_list
        return bdev_section, 0

    def gen_fio_filename_conf(self, remote_subsystem_list, filename=None):
        subsystems = [str(x) for x in range(0, len(remote_subsystem_list))]
        header_prefix = "filename"
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
            header = "[" + header_prefix + str(t) + "]"
            if not filename:
                disks = "\n".join(["filename=Nvme%sn1" % x for x in subsystems[n * t:n + n * t]])
            else:
                disks = filename[t]
            filename_section = "\n".join([filename_section, header, disks])

        return filename_section


def initiator_run_fio(initiator_obj, target_obj, case, type_rw=None):
    target_results_dir = "/tmp/results"
    postfix = case
    if not os.path.exists("/tmp/results"):
        out = subprocess.check_output("mkdir /tmp/results", shell=True).decode(encoding="utf-8")
    if os.path.exists("/tmp/results/" + postfix):
        subprocess.check_output("rm -rf /tmp/results/" + postfix, shell=True).decode(encoding="utf-8")
    subprocess.check_output("mkdir /tmp/results/" + postfix, shell=True).decode(encoding="utf-8")
    target_results_dir = target_results_dir + "/" + postfix
    for block_size, io_depth, rw in fio_workloads:
        block_size = block_size[0]
        io_depth = io_depth[0]
        rw = rw[0]
        if type_rw:
            rw = type_rw
        cfg = initiator_obj.iscsi_fio_config(block_size, io_depth, rw, fio_run_time, fio_num_jobs, 0)
        outcome = initiator_obj.run_fio(cfg, fio_run_num)
        if not outcome:
            return False
        if target_obj.enable_sar:
            sar_file_name = "_".join([str(block_size), str(rw), str(io_depth), "sar"])
            sar_file_name = ".".join([sar_file_name, "txt"])
            initiator_obj.measure_sar(target_results_dir, sar_file_name)
        initiator_obj.copy_result_files(target_results_dir)

    target_obj.parse_results(target_results_dir)
    return True


def judging_results(result, initiator_obj, case, f):
    if result:
        result_dict[case] = "pass"
        initiator_obj.log_print("-----------end the %s :successful -----------" % case)
        f.write("-----------the %s :pass -----------\r\n" % case)
        f.write("\n")
    else:
        result_dict[case] = "failed"
        initiator_obj.log_print("-----------end the %s :failed -----------" % case)
        f.write("-----------the %s :failed -----------\r\n" % case)
        f.write("\n")


def previously_results(case, f):
    f.write('----------- %s -----------\n' % case)
    if case == "case_1":
        f.write("initiator-a:  sg_persist --out --register --param-sark=123abc /dev/sdb\n")
        f.write("initiator-a/b/c : sg_persist --in --read-keys --device=/dev/sdb\n")
        f.write("initiator-a/b/c : read keys value should be initiator-a register param-sark:123abc \n")
    elif case == "case_2":
        f.write("initiator-a:  sg_persist --out --register --param-sark=123abc /dev/sdb\n")
        f.write("initiator-a:  sg_persist --out --register-ignore --param-sark=abc123 /dev/sdb\n")
        f.write("initiator-a/b/c : sg_persist --in --read-keys --device=/dev/sdb\n")
        f.write("initiator-a/b/c : read keys value should be initiator-a register-ignore param-sark:abc123 \n")
    elif case == "case_3":
        f.write("initiator-a/b/c:  sg_persist --out --register --param-sark=123abc/456abc/789abc /dev/sdb\n")
        f.write("initiator-a : sg_persist --out --reserve --prout-type=5 --param-rk=123abc /dev/sdb\n")
        f.write("initiator-a : run fio read/write testing to test the iscsi disk, read/write is except ok\n")
    elif case == "case_4":
        f.write("initiator-a/b/c:  sg_persist --out --register --param-sark=123abc/456abc/789abc /dev/sdb\n")
        f.write("initiator-a : sg_persist --out --reserve --prout-type=5 --param-rk=123abc /dev/sdb\n")
        f.write("initiator-b/c: run fio read/write testing to test the iscsi disk, read/write is except ok\n")
    elif case == "case_5":
        f.write("initiator-a/b:  sg_persist --out --register --param-sark=123abc/456abc /dev/sdb\n")
        f.write("initiator-a : sg_persist --out --reserve --prout-type=5 --param-rk=123abc /dev/sdb\n")
        f.write("initiator-c : run fio read/write testing to test the iscsi disk. read ok but write operation should fail.\n")
    elif case == "case_6":
        f.write("initiator-a/b:  sg_persist --out --register --param-sark=123abc/456abc /dev/sdb\n")
        f.write("initiator-a : sg_persist --out --reserve --prout-type=5 --param-rk=123abc /dev/sdb\n")
        f.write(
            "initiator-b : sg_persist --out --reserve --prout-type=5 --param-rk=456abc /dev/sdb --- this step should fail\n")
    elif case == "case_7":
        f.write("initiator-a/b:  sg_persist --out --register --param-sark=123abc/456abc /dev/sdb\n")
        f.write("initiator-a : sg_persist --out --reserve --prout-type=5 --param-rk=123abc /dev/sdb\n")
        f.write(
            "initiator-c : sg_persist --out --reserve --prout-type=5 --param-rk=456abc /dev/sdb --- this step should fail\n")
    elif case == "case_8":
        f.write("initiator-a/b:  sg_persist --out --register --param-sark=123abc/456abc /dev/sdb\n")
        f.write("initiator-a : sg_persist --out --reserve --prout-type=5 --param-rk=123abc /dev/sdb\n")
        f.write(
            "initiator-b : sg_persist --out --release --prout-type=5 --param-rk=456abc /dev/sdb --- this step should pass, but reservation is not released.\n")
    elif case == "case_9":
        f.write("initiator-a/b:  sg_persist --out --register --param-sark=123abc/456abc /dev/sdb\n")
        f.write("initiator-a : sg_persist --out --reserve --prout-type=5 --param-rk=123abc /dev/sdb\n")
        f.write(
            "initiator-c : sg_persist --out --release --prout-type=5 --param-rk=456abc /dev/sdb --- this step should fail, can't release.\n")
    elif case == "case_10":
        f.write("initiator-a/b:  sg_persist --out --register --param-sark=123abc/456abc /dev/sdb\n")
        f.write("initiator-a : sg_persist --out --reserve --prout-type=5 --param-rk=123abc /dev/sdb\n")
        f.write(
            "initiator-a : sg_persist --out --clear --prout-type=5 --param-rk=123abc /dev/sdb\n")
        f.write("initiator-a :  sg_persist --in -k  --device=/dev/sdb  --- this step should be successsful, the keys has cleared\n")
    elif case == "case_11":
        f.write("initiator-a/b:  sg_persist --out --register --param-sark=123abc/456abc /dev/sdb\n")
        f.write("initiator-a : sg_persist --out --reserve --prout-type=5 --param-rk=123abc /dev/sdb\n")
        f.write(
            "initiator-b : sg_persist --out --clear --prout-type=5 --param-rk=123abc /dev/sdb\n")
        f.write(
            "initiator-a :  sg_persist --in -k  --device=/dev/sdb  --- this step should be successsful, the keys has cleared\n")
    elif case == "case_12":
        f.write("initiator-a/b:  sg_persist --out --register --param-sark=123abc/456abc /dev/sdb\n")
        f.write("initiator-a : sg_persist --out --reserve --prout-type=5 --param-rk=123abc /dev/sdb\n")
        f.write(
            "initiator-c : sg_persist --out --clear --prout-type=5 --param-rk=123abc /dev/sdb --- this step should be fail, can't clear the keys\n")
    elif case == "case_13":
        f.write("initiator-a:  sg_persist --out --register --param-sark=123abc /dev/sdb\n")
        f.write("initiator-a : sg_persist --out --reserve --prout-type=5 --param-rk=123abc /dev/sdb\n")
        f.write(
            "initiator-b : sg_persist --out --preempt --prout-type=5 --param-rk=456abc --param-sark=123abc /dev/sdb --- this step should fail, can't preempt\n")
    elif case == "case_14":
        f.write("initiator-a/b:  sg_persist --out --register --param-sark=123abc/456abc /dev/sdb\n")
        f.write("initiator-a : sg_persist --out --reserve --prout-type=5 --param-rk=123abc /dev/sdb\n")
        f.write(
            "initiator-b : sg_persist --out --preempt --prout-type=5 --param-rk=456abc --param-sark=123abc /dev/sdb --- this step should fail, can't preempt\n")
        f.write("initiator-b : run fio write testing to test the iscsi disk. write operation should ok.\n")
    elif case == "case_15":
        f.write("initiator-a/b:  sg_persist --out --register --param-sark=123abc/456abc /dev/sdb\n")
        f.write("initiator-a : sg_persist --out --reserve --prout-type=5 --param-rk=123abc /dev/sdb\n")
        f.write(
            "initiator-b : sg_persist --out --preempt --prout-type=5 --param-rk=456abc --param-sark=123abc /dev/sdb --- this step should fail, can't preempt\n")
        f.write(
            "initiator-b : run fio read/write testing to test the iscsi disk. write operation should ok.\n")
        f.write("initiator-a:  sg_persist --out --register --param-sark=123abc /dev/sdb\n")
        f.write("initiator-a : sg_persist --out --preempt --prout-type=5 --param-rk=123abc --param-sark=456abc /dev/sdb\n")
        f.write(
            "initiator-a : run fio write testing to test the iscsi disk. write operation should ok.\n")
        f.write(
            "initiator-b : run fio write testing to test the iscsi disk. write operation should fail.\n")
    f.write('\n')


def case_1(initiators, case):
    result = True
    register_value = "123abc"
    for i, initiator_obj in enumerate(initiators):
        initiator_obj.iscsi_login()
        device_paths = initiator_obj.get_device_path()
        if i + 1 == 1:
            initiator_obj.log_print("*********************************************************************************")
            initiator_obj.log_print("-----------start to test %s-----------" % case)
            previously_results(case, f)
            reg = initiator_obj.reserve_register_key(register_value, device_paths)
            if case == "case_2":
                register_value = "abc123"
                reg = initiator_obj.reserve_register_ignore(register_value, device_paths)
            if reg:
                register_1, keys_1 = initiator_obj.reserve_read_keys(device_paths)
                if not register_1 or not keys_1:
                    initiator_obj.log_print("initiator_a read keys failed")
                    f.write("initiator_a read keys failed\n")
                    result = False
                elif keys_1[0] != register_value:
                    initiator_obj.log_print("initiator_a read keys false")
                    initiator_obj.log_print(
                        "initiator_a read keys is [%s], the true keys is [%s]" % (keys_1[0], register_value))
                    f.write("initiator_a read keys failed\n")
                    result = False
            else:
                initiator_obj.log_print("initiator_a register failed")
                f.write("initiator_a register failed\n")
                result = False
        elif i + 1 == 2:
            register_2, keys_2 = initiator_obj.reserve_read_keys(device_paths)
            if not register_2 or not keys_2:
                initiator_obj.log_print("initiator_b read keys failed")
                f.write("initiator_b read keys failed\n")
                result = False
            elif keys_2[0] != register_value:
                initiator_obj.log_print("initiator_b read keys false")
                f.write("initiator_b read keys failed\n")
                initiator_obj.log_print(
                    "initiator_b read keys is [%s], the true keys is [%s]" % (keys_2[0], register_value))
                result = False
        elif i + 1 == 3:
            register_3, keys_3 = initiator_obj.reserve_read_keys(device_paths)
            if not register_3 or not keys_3:
                initiator_obj.log_print("initiator_c read keys failed")
                f.write("initiator_c read keys failed\n")
                result = False
            elif keys_3[0] != register_value:
                initiator_obj.log_print("initiator_c read keys false")
                f.write("initiator_c read keys failed\n")
                initiator_obj.log_print(
                    "initiator_c read keys is [%s], the true keys is [%s]" % (keys_3[0], register_value))
                result = False
            judging_results(result, initiator_obj, case, f)

            initiator_obj.log_print("*********************************************************************************")
        else:
            pass

    for initiator_obj in initiators:
        iscsi_logout_delete = "iscsiadm -m node --logout && iscsiadm -m node -o delete"
        initiator_obj.remote_call(iscsi_logout_delete)


def case_2(initiators):
    case_1(initiators, "case_2")


def case_3(initiators, target_obj, case):
    result = True
    value_a = ''
    device_a = ''
    for i, initiator_obj in enumerate(initiators):
        initiator_obj.iscsi_login()
        device_paths = initiator_obj.get_device_path()
        register_value = ""
        initiator_name = ""
        if i+1 == 1:
            initiator_obj.log_print("*********************************************************************************")
            initiator_obj.log_print("-----------start to test %s-----------" % case)
            register_value = "123abc"
            initiator_name = "initiator_a"
            value_a = register_value
            device_a = device_paths
            previously_results(case, f)
        elif i+1 == 2:
            register_value = "456abc"
            initiator_name = "initiator_b"
        elif i+1 == 3:
            register_value = "789abc"
            initiator_name = "initiator_c"
        reg = initiator_obj.reserve_register_key(register_value, device_paths)
        if not reg:
            result = False
            initiator_obj.log_print("%s register failed" % initiator_name)
            initiator_obj.log_print("-----------end the %s :failed -----------" % case)

    if result:
        for i, initiator_obj in enumerate(initiators):
            if i+1 == 1:
                outcome = initiator_obj.reserve_prout_type(5, value_a, device_a)
                if not outcome:
                    result = False
                if case == "case_3":
                    out_come = initiator_run_fio(initiator_obj, target_obj, case)
                    if out_come:
                        initiator_obj.log_print("initiator_a run fio ok")
                        f.write("initiator_a run fio ok\n")
                    else:
                        initiator_obj.log_print("initiator_a run fio failed")
                        f.write("initiator_a run fio failed\n")
                        result = False

                    if not initiator_obj.reserve_clear_prout_type(5, value_a, device_a):
                        result = False
                    judging_results(result, initiator_obj, case, f)
                    break
                else:
                    continue
            elif i+1 == 2:
                out_come = initiator_run_fio(initiator_obj, target_obj, case)
                if out_come:
                    initiator_obj.log_print("initiator_b run fio ok")
                    f.write("initiator_b run fio ok\n")
                else:
                    initiator_obj.log_print("initiator_b run fio failed")
                    f.write("initiator_b run fio failed\n")
                    result = False
            elif i+1 == 3:
                out_come = initiator_run_fio(initiator_obj, target_obj, case)
                if out_come:
                    initiator_obj.log_print("initiator_c run fio ok")
                    f.write("initiator_c run fio ok\n")
                else:
                    initiator_obj.log_print("initiator_c run fio failed")
                    f.write("initiator_c run fio failed\n")
                    result = False
                judging_results(result, initiator_obj, case, f)
            else:
                pass
        if case == "case_4":
            initiators[0].reserve_clear_prout_type(5, value_a, device_a)

    for initiator_obj in initiators:
        iscsi_logout_delete = "iscsiadm -m node --logout && iscsiadm -m node -o delete"
        initiator_obj.remote_call(iscsi_logout_delete)


def case_4(initiators, target_obj):
    case_3(initiators, target_obj, "case_4")


def case_5(initiators, cases, target_obj=None):
    result = True
    fio_read = True
    fio_write = True
    case = cases
    value_a = ''
    device_a = ''
    for i, initiator_obj in enumerate(initiators):
        initiator_obj.iscsi_login()
        device_paths = initiator_obj.get_device_path()
        if i+1 == 1:
            initiator_obj.log_print("*********************************************************************************")
            initiator_obj.log_print("-----------start to test %s-----------" % case)
            previously_results(case, f)
            register_value = "123abc"
            initiator_name = "initiator_a"
            value_a = register_value
            device_a = device_paths
            reg = initiator_obj.reserve_register_key(register_value, device_paths)
            if not reg:
                result = False
                initiator_obj.log_print("%s register failed" % initiator_name)
                f.write("%s register failed\n" % initiator_name)
            outcome = initiator_obj.reserve_prout_type(5, register_value, device_paths)
            if not outcome:
                result = False
                f.write("%s reserve_prout_type=5 failed\n" % initiator_name)
            if case == "case_10":
                outcome = initiator_obj.reserve_clear_prout_type(5, value_a, device_a)
                if outcome:
                    out_come = initiator_obj.reserve_query_prout_type(device_paths)
                if not outcome:
                    result = False
                    f.write("%s sg_persist --in -k  failed\n" % initiator_name)
                judging_results(result, initiator_obj, case, f)
                break
        elif i+1 == 2:
            register_value = "456abc"
            initiator_name = "initiator_b"
            reg = initiator_obj.reserve_register_key(register_value, device_paths)
            if not reg:
                result = False
                initiator_obj.log_print("%s register failed" % initiator_name)
                f.write("%s register failed\n" % initiator_name)
            if case == "case_6":
                outcome = initiator_obj.reserve_prout_type(5, register_value, device_paths)
                if outcome:
                    result = False
                    f.write("%s reserve_prout_type=5 ok\n" % initiator_name)
                judging_results(result, initiator_obj, case, f)
            elif case == "case_8":
                outcome = initiator_obj.reserve_release_prout_type(5, register_value, device_paths)
                if not outcome:
                    result = False
                    f.write("%s release_prout_type failed\n" % initiator_name)
                judging_results(result, initiator_obj, case, f)
            elif case == "case_11":
                outcome = initiator_obj.reserve_clear_prout_type(5, register_value, device_paths)
                if not outcome:
                    result = False
                    f.write("%s clear_prout_type failed\n" % initiator_name)
        elif i+1 == 3:
            register_value = "789abc"
            if case == "case_5":
                out_come = initiator_run_fio(initiator_obj, target_obj, case, "read")
                if out_come:
                    initiator_obj.log_print("initiator_c run fio [read] ok")
                    f.write("initiator_c run fio [read] ok\n")
                else:
                    initiator_obj.log_print("initiator_c run fio [read] failed")
                    fio_read = False
                    f.write("initiator_c run fio [read] failed\n")
                outcome = initiator_run_fio(initiator_obj, target_obj, case, "write")
                if outcome:
                    initiator_obj.log_print("initiator_c run fio [write] ok")
                    f.write("initiator_c run fio [write] ok\n")
                else:
                    initiator_obj.log_print("initiator_c run fio [write] failed")
                    fio_write = False
                    f.write("initiator_c run fio [write] failed\n")
                if result:
                    if fio_read and not fio_write:
                        result_dict[case] = "pass"
                        initiator_obj.log_print("-----------end the %s :successful -----------" % case)
                        f.write("-----------the %s :pass -----------\n" % case)
                        f.write("\r\n")
                    else:
                        result_dict[case] = "failed"
                        initiator_obj.log_print("-----------end the %s :failed -----------" % case)
                        f.write("-----------the %s :failed -----------\n" % case)
                        f.write("\r\n")
                else:
                    result_dict[case] = "failed"
                    initiator_obj.log_print("-----------end the %s :failed -----------" % case)
                    f.write("-----------the %s :failed -----------\n" % case)
                    f.write("\r\n")
            elif case == "case_7":
                outcome = initiator_obj.reserve_prout_type(5, register_value, device_paths)
                if outcome:
                    result = False
                    f.write("initiator_c reserve_prout_type=5 ok\n")
                judging_results(result, initiator_obj, case, f)
            elif case == "case_9":
                outcome = initiator_obj.reserve_release_prout_type(5, register_value, device_paths)
                if outcome:
                    result = False
                    f.write("initiator_c release_prout_type ok\n")
                judging_results(result, initiator_obj, case, f)
            elif case == "case_12":
                outcome = initiator_obj.reserve_clear_prout_type(5, register_value, device_paths)
                if outcome:
                    result = False
                    f.write("initiator_c clear_prout_type ok\n")
                judging_results(result, initiator_obj, case, f)
            else:
                pass
        else:
            pass
    if case == "case_11":
        outcome = initiators[0].reserve_query_prout_type(device_a)
        if not outcome:
            result = False
            f.write("initiator_a sg_persist --in -k  failed\n")
        judging_results(result, initiators[0], case, f)
    if case != "case_10" or case != "case_11":
        initiators[0].reserve_clear_prout_type(5, value_a, device_a)
    for initiator_obj in initiators:
        iscsi_logout_delete = "iscsiadm -m node --logout && iscsiadm -m node -o delete"
        initiator_obj.remote_call(iscsi_logout_delete)


def case_13(initiators, cases, target_obj=None):
    result = True
    case = cases
    value_a = ''
    device_a = ''
    value_b = ''
    for i, initiator_obj in enumerate(initiators):
        initiator_obj.iscsi_login()
        device_paths = initiator_obj.get_device_path()
        if i+1 == 1:
            initiator_obj.log_print("*********************************************************************************")
            if case == "case_15":
                initiator_obj.log_print("-----------start to test case_14 and %s-----------" % case)
            else:
                initiator_obj.log_print("-----------start to test %s-----------" % case)
            previously_results(case, f)
            register_value = "123abc"
            initiator_name = "initiator_a"
            value_a = register_value
            device_a = device_paths
            reg = initiator_obj.reserve_register_key(register_value, device_paths)
            if not reg:
                result = False
                initiator_obj.log_print("%s register failed" % initiator_name)
                f.write("%s register failed\n" % initiator_name)
            outcome = initiator_obj.reserve_prout_type(5, register_value, device_paths)
            if not outcome:
                result = False
                f.write("%s reserve_prout_type=5 failed\n" % initiator_name)
        elif i+1 == 2:
            register_value = "456abc"
            initiator_name = "initiator_b"
            value_b = register_value
            device_b = device_paths
            if case == "case_13":
                outcome = initiator_obj.reserve_preempt_prout_type(5, register_value, value_a, device_paths)
                if outcome:
                    result = False
                    f.write("%s reserve_preempt_prout_type ok\n" % initiator_name)
                judging_results(result, initiator_obj, case, f)
            elif case == "case_15":
                reg = initiator_obj.reserve_register_key(register_value, device_paths)
                if not reg:
                    result = False
                    initiator_obj.log_print("%s register failed" % initiator_name)
                    f.write("%s register failed\n" % initiator_name)
                outcome = initiator_obj.reserve_preempt_prout_type(5, register_value, value_a, device_paths)
                if not outcome:
                    result = False
                    f.write("%s reserve_preempt_prout_type failed\n" % initiator_name)
                outcome = initiator_run_fio(initiator_obj, target_obj, case, "write")
                if outcome:
                    initiator_obj.log_print("initiator_b run fio [write] ok")
                    f.write("initiator_b run fio [write] ok\n")
                else:
                    initiator_obj.log_print("initiator_b run fio [write] failed")
                    result = False
                    f.write("initiator_b run fio [write] failed\n")
                judging_results(result, initiator_obj, "case_14", f)
                if not result:
                    result_dict[case] = "failed"
                    initiator_obj.log_print("-----------end the %s :failed -----------" % case)
                    f.write("-----------the %s :failed -----------\n" % case)
        else:
            pass
    if case == "case_15":
        for i, initiator_obj in enumerate(initiators):
            initiator_obj.iscsi_login()
            device_paths = initiator_obj.get_device_path()
            if i+1 == 1:
                initiator_name = "initiator_a"
                reg = initiator_obj.reserve_register_key(value_a, device_paths)
                if not reg:
                    result = False
                    initiator_obj.log_print("%s register failed" % initiator_name)
                    f.write("%s register failed\n" % initiator_name)
                outcome = initiator_obj.reserve_preempt_prout_type(5, value_a, value_b, device_paths)
                if not outcome:
                    result = False
                    f.write("%s reserve_preempt_prout_type failed\n" % initiator_name)
                outcome = initiator_run_fio(initiator_obj, target_obj, case, "write")
                if outcome:
                    initiator_obj.log_print("initiator_a run fio [write] ok")
                    f.write("initiator_a run fio [write] ok\n")
                else:
                    initiator_obj.log_print("initiator_a run fio [write] failed")
                    result = False
                    f.write("initiator_a run fio [write] failed\n")
            elif i+1 == 2:
                outcome = initiator_run_fio(initiator_obj, target_obj, case, "write")
                if outcome:
                    initiator_obj.log_print("initiator_b run fio [write] ok")
                    result = False
                    f.write("initiator_b run fio [write] ok\n")
                else:
                    initiator_obj.log_print("initiator_b run fio [write] failed")
                    f.write("initiator_b run fio [write] failed\n")
                judging_results(result, initiator_obj, case, f)
            else:
                pass
    initiators[0].reserve_clear_prout_type(5, value_a, device_a)
    for initiator_obj in initiators:
        iscsi_logout_delete = "iscsiadm -m node --logout && iscsiadm -m node -o delete"
        initiator_obj.remote_call(iscsi_logout_delete)



def case_6(initiators):
    case_5(initiators, "case_6")


def case_7(initiators):
    case_5(initiators, "case_7")


def case_8(initiators):
    case_5(initiators, "case_8")


def case_9(initiators):
    case_5(initiators, "case_9")


def case_10(initiators):
    case_5(initiators, "case_10")


def case_11(initiators):
    case_5(initiators, "case_11")


def case_12(initiators):
    case_5(initiators, "case_12")


def case_15(initiators, target_obj):
    case_13(initiators, "case_15", target_obj)



if __name__ == "__main__":
    spdk_zip_path = "/tmp/spdk.zip"
    run_no_case = 0
    if (len(sys.argv) > 1):
        config_file_path = sys.argv[1]
    else:
        script_full_dir = os.path.dirname(os.path.realpath(__file__))
        config_file_path = os.path.join(script_full_dir, "config.json")

    print("Using config file: %s" % config_file_path)
    with open(config_file_path, "r") as config:
        data = json.load(config)

    initiators = []
    fio_cases = []
    iti_nic_ips_list = []

    for k, v in data.items():
        if "target" in k:
            if data[k]["mode"] == "spdk":
                target_obj = SPDKTarget(name=k, **data["general"], **v)
        elif "initiator" in k:
            if data[k]["mode"] == "spdk":
                init_obj = SPDKInitiator(name=k, **data["general"], **v)
            initiators.append(init_obj)
        elif "fio" in k:
            print(data[k]["bs"], data[k]["qd"], data[k]["rw"], type(data[k]["bs"]), type(data[k]["qd"]), type(data[k]["rw"]))
            # fio_workloads = itertools.product(data[k]["bs"],
            #                                   data[k]["qd"],
            #                                   data[k]["rw"])
            fio_workloads = [(data[k]["bs"], data[k]["qd"], data[k]["rw"])]

            fio_rw_mix_read = data[k]["rwmixread"]
            fio_run_time = data[k]["run_time"] if "run_time" in data[k].keys() else 10
            fio_ramp_time = data[k]["ramp_time"] if "ramp_time" in data[k].keys() else 0
            fio_run_num = data[k]["run_num"] if "run_num" in data[k].keys() else None
            fio_num_jobs = data[k]["num_jobs"] if "num_jobs" in data[k].keys() else None
            fio_mem_size_mb = data[k]["mem_size_mb"] if "mem_size_mb" in data[k].keys() else None
            fio_size = data[k]["size"] if "size" in data[k].keys() else "8G"
            if "run_no_case" in data[k].keys():
                run_no_case = data[k]["run_no_case"]
        else:
            continue

    # Copy and install SPDK on remote initiators
    # target_obj.zip_spdk_sources(target_obj.spdk_dir, spdk_zip_path)
    # threads = []
    # for i in initiators:
    #     if i.mode == "spdk":
    #         t = threading.Thread(target=i.install_spdk, args=(spdk_zip_path,))
    #         threads.append(t)
    #         t.start()
    # for t in threads:
    #     t.join()

    target_obj.tgt_start(len(initiators))
    f = open(result_file, 'a')
    try:
        if run_no_case:
            for initiator_obj in initiators:
                initiator_obj.iscsi_login()
                out_come = initiator_run_fio(initiator_obj, target_obj, "nocase")
                if out_come:
                    initiator_obj.log_print("initiator_a run fio ok")
                    f.write("[%s] run fio ok\n" % initiator_obj.name)
                else:
                    initiator_obj.log_print("initiator_a run fio failed")
                    f.write("[%s] run fio failed\n" % initiator_obj.name)
                iscsi_logout_delete = "iscsiadm -m node --logout && iscsiadm -m node -o delete"
                initiator_obj.remote_call(iscsi_logout_delete)
        else:
            case_1(initiators, "case_1")
            case_2(initiators)
            case_3(initiators, target_obj, "case_3")
            case_4(initiators, target_obj)
            case_5(initiators, "case_5", target_obj)
            case_6(initiators)
            case_7(initiators)
            case_8(initiators)
            case_9(initiators)
            case_10(initiators)
            case_11(initiators)
            case_12(initiators)
            case_13(initiators, "case_13")
            case_15(initiators, target_obj)
        json_str = json.dumps(result_dict, indent=1)
        f.write(json_str)
        f.close()
    except BaseException as e:
        print(e)
        f.close()
        sys.exit(1)
