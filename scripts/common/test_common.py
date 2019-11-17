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
# from common import *
from perf.nvmf.common import *



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
    def __init__(self, name, username, password, mode, nic_ips, transport, log_file_name):
        self.name = name
        self.mode = mode
        self.username = username
        self.password = password
        self.nic_ips = nic_ips
        self.transport = transport.lower()
        self.log_file_name = log_file_name
        # if os.path.exists("/home/iscsi_log/run_iscsi.log"):
        #     proc = subprocess.Popen("rm -rf /home/iscsi_log/run_iscsi.log", shell=True)
        if not re.match("^[A-Za-z0-9]*$", name):
            self.log_print("Please use a name which contains only letters or numbers")
            sys.exit(1)

    def log_print(self, msg, source=None):
        log_name = self.log_file_name.split(r'/')[-1]
        self.logger = Logger(log_file_name=self.log_file_name, log_level=logging.DEBUG,
                             logger_name=log_name).get_log()
        if source:
            msg = json.dumps(msg, indent=2)
            msg = "[%s] %s" % (source, msg)
        self.logger.info("[%s] %s" % (self.name, msg))


class Target(Server):
    def __init__(self, name, username, password, mode, nic_ips, transport="rdma", use_null_block=False,
                 sar_settings=None, log_file_name=None):
        super(Target, self).__init__(name, username, password, mode, nic_ips, transport, log_file_name)
        self.null_block = bool(use_null_block)
        self.enable_sar = False
        if sar_settings:
            self.enable_sar, self.sar_delay, self.sar_interval, self.sar_count = sar_settings

        self.script_dir = os.path.dirname(os.path.abspath(sys.argv[0]))
        print("the script dir is ", self.script_dir)
        self.spdk_dir = os.path.abspath(os.path.join(self.script_dir, "../../"))
        self.tgt_dir = os.path.abspath(os.path.join(self.spdk_dir, "app"))
        if os.path.exists(self.tgt_dir):
           print("already found the spdk dir")
           pass
        else:
           print("need to upper folder.")
           self.spdk_dir = os.path.abspath(os.path.join(self.script_dir, "../../../"))
           self.tgt_dir = os.path.abspath(os.path.join(self.spdk_dir, "app"))
           print("the tgt dir is ", self.tgt_dir)

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
                read_avg_lat, read_min_lat, read_max_lat = [x / 1000 for x in
                                                            [read_avg_lat, read_min_lat, read_max_lat]]
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
                write_avg_lat, write_min_lat, write_max_lat = [x / 1000 for x in
                                                               [write_avg_lat, write_min_lat, write_max_lat]]
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
                z = [c / len(separate_stats) for c in z]
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
        out = subprocess.check_output("sar -P ALL %s %s" % (self.sar_interval, self.sar_count), shell=True).decode(
            encoding="utf-8")
        with open(os.path.join(results_dir, sar_file_name), "w") as fh:
            for line in out.split("\n"):
                if "Average" in line and "CPU" in line:
                    self.log_print("Summary CPU utilization from SAR:")
                    self.log_print(line)
                if "Average" in line and "all" in line:
                    self.log_print(line)
            fh.write(out)


class Initiator(Server):
    def __init__(self, name, username, password, mode, nic_ips, ip, transport="rdma", nvmecli_dir=None,
                 workspace="/tmp/spdk", fio_dir="/usr/src/fio", driver="bdev", log_file_name=None):
        super(Initiator, self).__init__(name, username, password, mode, nic_ips, transport, log_file_name)
        self.ip = ip
        self.spdk_dir = workspace
        self.driver = driver
        self.fio_dir = fio_dir
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
        print(address_list, "----------------", subsystems)
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

    def run_fio(self, fio_config_file, run_num=None):
        result = True
        job_name, _ = os.path.splitext(fio_config_file)
        self.log_print("Starting FIO run for job: %s" % job_name)
        fio_bin = os.path.join(self.fio_dir, "fio")
        self.log_print("Using FIO: %s" % fio_bin)
        if run_num:
            for i in range(1, run_num + 1):
                output_filename = job_name + "_run_" + str(i) + "_" + self.name + ".json"
                cmd = "sudo %s %s --output-format=json --output=%s" % (fio_bin, fio_config_file, output_filename)
                output, error = self.remote_call(cmd)
                self.log_print(output)
                self.log_print("11111111111111111111111111111")
                if error:
                    self.log_print(error)
                    result = False
        else:
            output_filename = job_name + "_" + self.name + ".json"
            cmd = "sudo %s %s --output-format=json --output=%s" % (fio_bin, fio_config_file, output_filename)
            output, error = self.remote_call(cmd)
            self.log_print(output)
            self.log_print("11111111111111111111111111111")
            if error:
                self.log_print(error)
                result = False
        self.log_print("FIO run finished. Results in: %s" % output_filename)
        return result

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
        fiofile = fio_template.format(blocksize=size, iodepth=q_depth, testtype=test, runtime=run_time,
                                      norandommap=norandommap, verify=verifyfio, numjobs=num_jobs)
        for (i, dev) in enumerate(devices):
            fiofile += fio_job_template % {"jobnumber": i, "device": dev}
        fio_config_filename = "%s_%s_%s_m_%s" % (size, q_depth, test, 100)
        if hasattr(self, "num_cores"):
            fio_config_filename += "_%sCPU" % self.num_cores
        fio_config_filename += ".fio"
        self.remote_call("mkdir -p %s/nvmf_perf" % self.spdk_dir)
        std, err = self.remote_call("echo '%s' > %s/nvmf_perf/%s" % (fiofile, self.spdk_dir, fio_config_filename))
        print(std, "-------------------------", err)
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
                self.log_print("[%s] reserve failed, prout-type=%d, [register can write, and only one PRs owner]" % (
                device, type_num))
                self.log_print(stderr)
                results = False
                break
            else:
                self.log_print(
                    "[%s] reserve ok, prout-type=%d, [register can write, and only one PRs owner]" % (device, type_num))
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
                self.log_print(
                    "[%s] reserve clear failed, prout-type=%d, [register can write, and only one PRs owner]" % (
                        device, type_num))
                self.log_print(stderr)
                results = False
                break
            else:
                self.log_print(
                    "[%s] reserve clear ok, prout-type=%d, [register can write, and only one PRs owner]" % (
                    device, type_num))
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
                    self.log_print(
                        "[%s] reserve query the clear keys ok,and there are NO registered reservation keys" % (device))
                    results = True
                    continue
        return results

    def reserve_preempt_prout_type(self, type_num, new_reg_value, old_reg_value, device_paths):
        # sg_persist --out --preempt --prout-type=5 --param-rk=456abc --param-sark=123abc /dev/sdb
        results = False
        for device in device_paths:
            preempt_cmd = "sg_persist --out --preempt --prout-type=%d --param-rk=%s --param-sark=%s %s" % (
            type_num, new_reg_value, old_reg_value, device)
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
        fio_config = self.iscsi_create_fio_config(io_size, queue_depth, device_paths, test_type, runtime, num_jobs,
                                                  verify)
        return fio_config


class KernelTarget(Target):
    def __init__(self, name, username, password, mode, nic_ips,
                 use_null_block=False, sar_settings=None, transport="rdma", nvmet_dir=None, log_file_name=None, **kwargs):
        super(KernelTarget, self).__init__(name, username, password, mode, nic_ips,
                                           transport, use_null_block, sar_settings, log_file_name=log_file_name)

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
                 use_null_block=False, sar_settings=None, transport="rdma", log_file_name=None, **kwargs):
        super(SPDKTarget, self).__init__(name, username, password, mode, nic_ips, transport, use_null_block,
                                         sar_settings, log_file_name=log_file_name)
        self.num_cores = num_cores
        self.num_shared_buffers = num_shared_buffers

    def spdk_tgt_configure(self):
        self.log_print("Configuring SPDK NVMeOF target via RPC")
        numa_list = get_used_numa_nodes()

        # Create RDMA transport layer
        rpc.nvmf.nvmf_create_transport(self.client, trtype=self.transport, num_shared_buffers=self.num_shared_buffers)
        self.log_print("SPDK NVMeOF transport layer:")
        self.log_print(rpc.nvmf.nvmf_get_transports(self.client), "rpc.nvmf")

        if self.null_block:
            self.spdk_tgt_add_nullblock()
            self.spdk_tgt_add_subsystem_conf(self.nic_ips, req_num_disks=1)
        else:
            if "filesystem" in self.log_file_name.split(r'/')[2]:
                self.spdk_tgt_add_nvme_conf(4)
                self.spdk_tgt_add_subsystem_conf(self.nic_ips, req_num_disks=4)
            else:
                self.spdk_tgt_add_nvme_conf()
                self.spdk_tgt_add_subsystem_conf(self.nic_ips)
        self.log_print("Done configuring SPDK NVMeOF Target")

    def spdk_iscsi_tgt_configure(self, initiator_num):
        self.log_print("Configuring SPDK iscsi target via RPC")
        numa_list = get_used_numa_nodes()

        # Create target group
        rpc.iscsi.iscsi_create_portal_group(self.client, [{'host': self.nic_ips[0], 'port': "3260"}], 1)
        # Create target group
        net_mask = self.nic_ips[0].split('.')
        net_mask[-1] = "0/24"
        net_mask = (".").join(net_mask)
        for i in range(initiator_num):
            rpc.iscsi.iscsi_create_initiator_group(self.client, (i + 1), ["Any"], [net_mask])
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
        if "iscsi" in self.log_file_name.split(r'/')[2]:
            luns_list = []
            for i, bdf in enumerate(bdfs):
                # rpc.bdev.construct_nvme_bdev(self.client, name="Nvme%s" % i, trtype="PCIe", traddr=bdf)
                rpc.bdev.bdev_nvme_attach_controller(self.client, name="Nvme%s" % i, trtype="PCIe", traddr=bdf)
                bdev_dict = {"bdev_name": "Nvme%s" % i + "n1", "lun_id": 0}
                luns_list.append(bdev_dict)
            tgt_name = "Target" + str(len(luns_list))
            tgt_alias = tgt_name + "_alias"
            rpc.iscsi.iscsi_create_target_node(self.client, luns_list, [{"pg_tag": 1, "ig_tag": 1}], tgt_name, tgt_alias,
                                               64, disable_chap=True)
        else:
            for i, bdf in enumerate(bdfs):
                # rpc.bdev.construct_nvme_bdev(self.client, name="Nvme%s" % i, trtype="PCIe", traddr=bdf)
                rpc.bdev.bdev_nvme_attach_controller(self.client, name="Nvme%s" % i, trtype="PCIe", traddr=bdf)
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

                if "filesystem" in self.log_file_name.split(r'/')[2]:
                    rpc.nvmf.nvmf_subsystem_add_listener(self.client, nqn,
                                                         trtype=self.transport,
                                                         traddr=ip,
                                                         trsvcid=str(4420),
                                                         adrfam="ipv4")
                else:
                    rpc.nvmf.nvmf_subsystem_add_listener(self.client, nqn,
                                                         trtype=self.transport,
                                                         traddr=ip,
                                                         trsvcid=str(4420 + c-1),
                                                         adrfam="ipv4")

        self.log_print("SPDK NVMeOF subsystem configuration:")
        # rpc.client.print_dict(rpc.nvmf.get_nvmf_subsystems(self.client))
        # rpc.client.print_dict(rpc.nvmf.nvmf_get_subsystems(self.client))
        self.log_print(rpc.nvmf.nvmf_get_subsystems(self.client), "rpc.nvmf")

    def tgt_start(self, initiator_num=None):
        self.subsys_no = get_nvme_devices_count()
        if "iscsi" in self.log_file_name.split(r'/')[2]:
            self.log_print("--------------Starting SPDK iscsi Target process--------------")
            app_path = os.path.join(self.spdk_dir, "app/iscsi_tgt/iscsi_tgt")
        else:
            self.log_print("--------------Starting SPDK NVMeOF Target process--------------")
            # nvmf_app_path = os.path.join(self.spdk_dir, "app/nvmf_tgt/nvmf_tgt &")
            app_path = os.path.join(self.spdk_dir, "app/nvmf_tgt/nvmf_tgt")
        command = " ".join([app_path, "-m", self.num_cores])
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

        if "iscsi" in self.log_file_name.split(r'/')[2]:
            self.spdk_iscsi_tgt_configure(initiator_num)
        else:
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
    def __init__(self, name, username, password, mode, nic_ips, ip, transport, nvmecli_dir, fio_dir, **kwargs):
        super(KernelInitiator, self).__init__(name, username, password, mode, nic_ips, ip, transport, nvmecli_dir, fio_dir)

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
    def __init__(self, name, username, password, mode, nic_ips, ip, num_cores=None, transport="rdma", nvmecli_dir=None,
                 workspace="/tmp/spdk",fio_dir=None, driver="bdev", log_file_name=None, **kwargs):
        super(SPDKInitiator, self).__init__(name, username, password, mode, nic_ips, ip, transport, nvmecli_dir, workspace, fio_dir, driver=driver, log_file_name=log_file_name)
        if num_cores:
            self.num_cores = num_cores
        self.driver = driver
        if "filesystem" in self.log_file_name.split(r'/')[1]:
            self.remote_call("rm -rf /home/nvmfpart0*")

    def install_spdk(self, local_spdk_zip):
        self.put_file(local_spdk_zip, "/tmp/spdk_drop.zip")
        self.log_print("Copied sources zip from target")
        self.remote_call("unzip -qo /tmp/spdk_drop.zip -d %s" % self.spdk_dir)

        self.log_print("Sources unpacked")
        self.log_print("Using fio directory %s" % self.fio_dir)
        self.remote_call("cd %s; git submodule update --init; ./configure --with-rdma --with-fio=%s;"
                         "make clean; make -j$(($(nproc)*2))" % (self.spdk_dir, self.fio_dir))

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
