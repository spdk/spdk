#!/usr/bin/env python3

from json.decoder import JSONDecodeError
import os
import re
import sys
import argparse
import json
import zipfile
import threading
import subprocess
import itertools
import configparser
import time
import uuid
from collections import OrderedDict

import paramiko
import pandas as pd

import rpc
import rpc.client
from common import *


class Server:
    def __init__(self, name, general_config, server_config):
        self.name = name
        self.username = general_config["username"]
        self.password = general_config["password"]
        self.transport = general_config["transport"].lower()
        self.nic_ips = server_config["nic_ips"]
        self.mode = server_config["mode"]

        self.irq_scripts_dir = "/usr/src/local/mlnx-tools/ofed_scripts"
        if "irq_scripts_dir" in server_config and server_config["irq_scripts_dir"]:
            self.irq_scripts_dir = server_config["irq_scripts_dir"]

        self.local_nic_info = []
        self._nics_json_obj = {}
        self.svc_restore_dict = {}
        self.sysctl_restore_dict = {}
        self.tuned_restore_dict = {}
        self.governor_restore = ""
        self.tuned_profile = ""

        self.enable_adq = False
        self.adq_priority = None
        if "adq_enable" in server_config and server_config["adq_enable"]:
            self.enable_adq = server_config["adq_enable"]
            self.adq_priority = 1

        if "tuned_profile" in server_config:
            self.tuned_profile = server_config["tuned_profile"]

        if not re.match("^[A-Za-z0-9]*$", name):
            self.log_print("Please use a name which contains only letters or numbers")
            sys.exit(1)

    def log_print(self, msg):
        print("[%s] %s" % (self.name, msg), flush=True)

    def get_uncommented_lines(self, lines):
        return [line for line in lines if line and not line.startswith('#')]

    def get_nic_name_by_ip(self, ip):
        if not self._nics_json_obj:
            nics_json_obj = self.exec_cmd(["ip", "-j", "address", "show"])
            self._nics_json_obj = list(filter(lambda x: x["addr_info"], json.loads(nics_json_obj)))
        for nic in self._nics_json_obj:
            for addr in nic["addr_info"]:
                if ip in addr["local"]:
                    return nic["ifname"]

    def set_local_nic_info_helper(self):
        pass

    def set_local_nic_info(self, pci_info):
        def extract_network_elements(json_obj):
            nic_list = []
            if isinstance(json_obj, list):
                for x in json_obj:
                    nic_list.extend(extract_network_elements(x))
            elif isinstance(json_obj, dict):
                if "children" in json_obj:
                    nic_list.extend(extract_network_elements(json_obj["children"]))
                if "class" in json_obj.keys() and "network" in json_obj["class"]:
                    nic_list.append(json_obj)
            return nic_list

        self.local_nic_info = extract_network_elements(pci_info)

    def exec_cmd(self, cmd, stderr_redirect=False, change_dir=None):
        return ""

    def configure_system(self):
        self.configure_services()
        self.configure_sysctl()
        self.configure_tuned()
        self.configure_cpu_governor()
        self.configure_irq_affinity()

    def configure_adq(self):
        if self.mode == "kernel":
            self.log_print("WARNING: ADQ setup not yet supported for Kernel mode. Skipping configuration.")
            return
        self.adq_load_modules()
        self.adq_configure_nic()

    def adq_load_modules(self):
        self.log_print("Modprobing ADQ-related Linux modules...")
        adq_module_deps = ["sch_mqprio", "act_mirred", "cls_flower"]
        for module in adq_module_deps:
            try:
                self.exec_cmd(["sudo", "modprobe", module])
                self.log_print("%s loaded!" % module)
            except CalledProcessError as e:
                self.log_print("ERROR: failed to load module %s" % module)
                self.log_print("%s resulted in error: %s" % (e.cmd, e.output))

    def adq_configure_tc(self):
        self.log_print("Configuring ADQ Traffic classes and filters...")

        if self.mode == "kernel":
            self.log_print("WARNING: ADQ setup not yet supported for Kernel mode. Skipping configuration.")
            return

        num_queues_tc0 = 2  # 2 is minimum number of queues for TC0
        num_queues_tc1 = self.num_cores
        port_param = "dst_port" if isinstance(self, Target) else "src_port"
        port = "4420"
        xps_script_path = os.path.join(self.spdk_dir, "scripts", "perf", "nvmf", "set_xps_rxqs")

        for nic_ip in self.nic_ips:
            nic_name = self.get_nic_name_by_ip(nic_ip)
            tc_qdisc_map_cmd = ["sudo", "tc", "qdisc", "add", "dev", nic_name,
                                "root", "mqprio", "num_tc", "2", "map", "0", "1",
                                "queues", "%s@0" % num_queues_tc0,
                                "%s@%s" % (num_queues_tc1, num_queues_tc0),
                                "hw", "1", "mode", "channel"]
            self.log_print(" ".join(tc_qdisc_map_cmd))
            self.exec_cmd(tc_qdisc_map_cmd)

            time.sleep(5)
            tc_qdisc_ingress_cmd = ["sudo", "tc", "qdisc", "add", "dev", nic_name, "ingress"]
            self.log_print(" ".join(tc_qdisc_ingress_cmd))
            self.exec_cmd(tc_qdisc_ingress_cmd)

            tc_filter_cmd = ["sudo", "tc", "filter", "add", "dev", nic_name,
                             "protocol", "ip", "ingress", "prio", "1", "flower",
                             "dst_ip", "%s/32" % nic_ip, "ip_proto", "tcp", port_param, port,
                             "skip_sw", "hw_tc", "1"]
            self.log_print(" ".join(tc_filter_cmd))
            self.exec_cmd(tc_filter_cmd)

            # show tc configuration
            self.log_print("Show tc configuration for %s NIC..." % nic_name)
            tc_disk_out = self.exec_cmd(["sudo", "tc", "qdisc", "show", "dev", nic_name])
            tc_filter_out = self.exec_cmd(["sudo", "tc", "filter", "show", "dev", nic_name, "ingress"])
            self.log_print("%s" % tc_disk_out)
            self.log_print("%s" % tc_filter_out)

            # Ethtool coalesce settings must be applied after configuring traffic classes
            self.exec_cmd(["sudo", "ethtool", "--coalesce", nic_name, "adaptive-rx", "off", "rx-usecs", "0"])
            self.exec_cmd(["sudo", "ethtool", "--coalesce", nic_name, "adaptive-tx", "off", "tx-usecs", "500"])

            self.log_print("Running set_xps_rxqs script for %s NIC..." % nic_name)
            xps_cmd = ["sudo", xps_script_path, nic_name]
            self.log_print(xps_cmd)
            self.exec_cmd(xps_cmd)

    def adq_configure_nic(self):
        self.log_print("Configuring NIC port settings for ADQ testing...")

        # Reload the driver first, to make sure any previous settings are re-set.
        try:
            self.exec_cmd(["sudo", "rmmod", "ice"])
            self.exec_cmd(["sudo", "modprobe", "ice"])
        except CalledProcessError as e:
            self.log_print("ERROR: failed to reload ice module!")
            self.log_print("%s resulted in error: %s" % (e.cmd, e.output))

        nic_names = [self.get_nic_name_by_ip(n) for n in self.nic_ips]
        for nic in nic_names:
            self.log_print(nic)
            try:
                self.exec_cmd(["sudo", "ethtool", "-K", nic,
                               "hw-tc-offload", "on"])  # Enable hardware TC offload
                self.exec_cmd(["sudo", "ethtool", "--set-priv-flags", nic,
                               "channel-inline-flow-director", "on"])  # Enable Intel Flow Director
                self.exec_cmd(["sudo", "ethtool", "--set-priv-flags", nic, "fw-lldp-agent", "off"])  # Disable LLDP
                # As temporary workaround for ADQ, channel packet inspection optimization is turned on during connection establishment.
                # Then turned off before fio ramp_up expires in ethtool_after_fio_ramp().
                self.exec_cmd(["sudo", "ethtool", "--set-priv-flags", nic,
                               "channel-pkt-inspect-optimize", "on"])
            except CalledProcessError as e:
                self.log_print("ERROR: failed to configure NIC port using ethtool!")
                self.log_print("%s resulted in error: %s" % (e.cmd, e.output))
                self.log_print("Please update your NIC driver and firmware versions and try again.")
            self.log_print(self.exec_cmd(["sudo", "ethtool", "-k", nic]))
            self.log_print(self.exec_cmd(["sudo", "ethtool", "--show-priv-flags", nic]))

    def configure_services(self):
        self.log_print("Configuring active services...")
        svc_config = configparser.ConfigParser(strict=False)

        # Below list is valid only for RHEL / Fedora systems and might not
        # contain valid names for other distributions.
        svc_target_state = {
            "firewalld": "inactive",
            "irqbalance": "inactive",
            "lldpad.service": "inactive",
            "lldpad.socket": "inactive"
        }

        for service in svc_target_state:
            out = self.exec_cmd(["sudo", "systemctl", "show", "--no-page", service])
            out = "\n".join(["[%s]" % service, out])
            svc_config.read_string(out)

            if "LoadError" in svc_config[service] and "not found" in svc_config[service]["LoadError"]:
                continue

            service_state = svc_config[service]["ActiveState"]
            self.log_print("Current state of %s service is %s" % (service, service_state))
            self.svc_restore_dict.update({service: service_state})
            if service_state != "inactive":
                self.log_print("Disabling %s. It will be restored after the test has finished." % service)
                self.exec_cmd(["sudo", "systemctl", "stop", service])

    def configure_sysctl(self):
        self.log_print("Tuning sysctl settings...")

        busy_read = 0
        if self.enable_adq and self.mode == "spdk":
            busy_read = 1

        sysctl_opts = {
            "net.core.busy_poll": 0,
            "net.core.busy_read": busy_read,
            "net.core.somaxconn": 4096,
            "net.core.netdev_max_backlog": 8192,
            "net.ipv4.tcp_max_syn_backlog": 16384,
            "net.core.rmem_max": 268435456,
            "net.core.wmem_max": 268435456,
            "net.ipv4.tcp_mem": "268435456 268435456 268435456",
            "net.ipv4.tcp_rmem": "8192 1048576 33554432",
            "net.ipv4.tcp_wmem": "8192 1048576 33554432",
            "net.ipv4.route.flush": 1,
            "vm.overcommit_memory": 1,
        }

        for opt, value in sysctl_opts.items():
            self.sysctl_restore_dict.update({opt: self.exec_cmd(["sysctl", "-n", opt]).strip()})
            self.log_print(self.exec_cmd(["sudo", "sysctl", "-w", "%s=%s" % (opt, value)]).strip())

    def configure_tuned(self):
        if not self.tuned_profile:
            self.log_print("WARNING: Tuned profile not set in configuration file. Skipping configuration.")
            return

        self.log_print("Configuring tuned-adm profile to %s." % self.tuned_profile)
        service = "tuned"
        tuned_config = configparser.ConfigParser(strict=False)

        out = self.exec_cmd(["sudo", "systemctl", "show", "--no-page", service])
        out = "\n".join(["[%s]" % service, out])
        tuned_config.read_string(out)
        tuned_state = tuned_config[service]["ActiveState"]
        self.svc_restore_dict.update({service: tuned_state})

        if tuned_state != "inactive":
            profile = self.exec_cmd(["cat", "/etc/tuned/active_profile"]).strip()
            profile_mode = self.exec_cmd(["cat", "/etc/tuned/profile_mode"]).strip()

            self.tuned_restore_dict = {
                "profile": profile,
                "mode": profile_mode
            }

        self.exec_cmd(["sudo", "systemctl", "start", service])
        self.exec_cmd(["sudo", "tuned-adm", "profile", self.tuned_profile])
        self.log_print("Tuned profile set to %s." % self.exec_cmd(["cat", "/etc/tuned/active_profile"]))

    def configure_cpu_governor(self):
        self.log_print("Setting CPU governor to performance...")

        # This assumes that there is the same CPU scaling governor on each CPU
        self.governor_restore = self.exec_cmd(["cat", "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"]).strip()
        self.exec_cmd(["sudo", "cpupower", "frequency-set", "-g", "performance"])

    def configure_irq_affinity(self):
        self.log_print("Setting NIC irq affinity for NICs...")

        irq_script_path = os.path.join(self.irq_scripts_dir, "set_irq_affinity.sh")
        nic_names = [self.get_nic_name_by_ip(n) for n in self.nic_ips]
        for nic in nic_names:
            irq_cmd = ["sudo", irq_script_path, nic]
            self.log_print(irq_cmd)
            self.exec_cmd(irq_cmd, change_dir=self.irq_scripts_dir)

    def restore_services(self):
        self.log_print("Restoring services...")
        for service, state in self.svc_restore_dict.items():
            cmd = "stop" if state == "inactive" else "start"
            self.exec_cmd(["sudo", "systemctl", cmd, service])

    def restore_sysctl(self):
        self.log_print("Restoring sysctl settings...")
        for opt, value in self.sysctl_restore_dict.items():
            self.log_print(self.exec_cmd(["sudo", "sysctl", "-w", "%s=%s" % (opt, value)]).strip())

    def restore_tuned(self):
        self.log_print("Restoring tuned-adm settings...")

        if not self.tuned_restore_dict:
            return

        if self.tuned_restore_dict["mode"] == "auto":
            self.exec_cmd(["sudo", "tuned-adm", "auto_profile"])
            self.log_print("Reverted tuned-adm to auto_profile.")
        else:
            self.exec_cmd(["sudo", "tuned-adm", "profile", self.tuned_restore_dict["profile"]])
            self.log_print("Reverted tuned-adm to %s profile." % self.tuned_restore_dict["profile"])

    def restore_governor(self):
        self.log_print("Restoring CPU governor setting...")
        if self.governor_restore:
            self.exec_cmd(["sudo", "cpupower", "frequency-set", "-g", self.governor_restore])
            self.log_print("Reverted CPU governor to %s." % self.governor_restore)


class Target(Server):
    def __init__(self, name, general_config, target_config):
        super(Target, self).__init__(name, general_config, target_config)

        # Defaults
        self.enable_sar = False
        self.sar_delay = 0
        self.sar_interval = 0
        self.sar_count = 0
        self.enable_pcm = False
        self.pcm_dir = ""
        self.pcm_delay = 0
        self.pcm_interval = 0
        self.pcm_count = 0
        self.enable_bandwidth = 0
        self.bandwidth_count = 0
        self.enable_dpdk_memory = False
        self.dpdk_wait_time = 0
        self.enable_zcopy = False
        self.scheduler_name = "static"
        self.null_block = 0
        self._nics_json_obj = json.loads(self.exec_cmd(["ip", "-j", "address", "show"]))
        self.subsystem_info_list = []

        if "null_block_devices" in target_config:
            self.null_block = target_config["null_block_devices"]
        if "sar_settings" in target_config:
            self.enable_sar, self.sar_delay, self.sar_interval, self.sar_count = target_config["sar_settings"]
        if "pcm_settings" in target_config:
            self.enable_pcm = True
            self.pcm_dir, self.pcm_delay, self.pcm_interval, self.pcm_count = target_config["pcm_settings"]
        if "enable_bandwidth" in target_config:
            self.enable_bandwidth, self.bandwidth_count = target_config["enable_bandwidth"]
        if "enable_dpdk_memory" in target_config:
            self.enable_dpdk_memory, self.dpdk_wait_time = target_config["enable_dpdk_memory"]
        if "scheduler_settings" in target_config:
            self.scheduler_name = target_config["scheduler_settings"]
        if "zcopy_settings" in target_config:
            self.enable_zcopy = target_config["zcopy_settings"]
        if "results_dir" in target_config:
            self.results_dir = target_config["results_dir"]

        self.script_dir = os.path.dirname(os.path.abspath(sys.argv[0]))
        self.spdk_dir = os.path.abspath(os.path.join(self.script_dir, "../../../"))
        self.set_local_nic_info(self.set_local_nic_info_helper())

        if "skip_spdk_install" not in general_config or general_config["skip_spdk_install"] is False:
            self.zip_spdk_sources(self.spdk_dir, "/tmp/spdk.zip")

        self.configure_system()
        if self.enable_adq:
            self.configure_adq()
        self.sys_config()

    def set_local_nic_info_helper(self):
        return json.loads(self.exec_cmd(["lshw", "-json"]))

    def exec_cmd(self, cmd, stderr_redirect=False, change_dir=None):
        stderr_opt = None
        if stderr_redirect:
            stderr_opt = subprocess.STDOUT
        if change_dir:
            old_cwd = os.getcwd()
            os.chdir(change_dir)
            self.log_print("Changing directory to %s" % change_dir)

        out = check_output(cmd, stderr=stderr_opt).decode(encoding="utf-8")

        if change_dir:
            os.chdir(old_cwd)
            self.log_print("Changing directory to %s" % old_cwd)
        return out

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
                for k, _ in dict_section.items():
                    if k.startswith(key_prefix):
                        return k, k.split("_")[1]

            def get_clat_percentiles(clat_dict_leaf):
                if "percentile" in clat_dict_leaf:
                    p99_lat = float(clat_dict_leaf["percentile"]["99.000000"])
                    p99_9_lat = float(clat_dict_leaf["percentile"]["99.900000"])
                    p99_99_lat = float(clat_dict_leaf["percentile"]["99.990000"])
                    p99_999_lat = float(clat_dict_leaf["percentile"]["99.999000"])

                    return [p99_lat, p99_9_lat, p99_99_lat, p99_999_lat]
                else:
                    # Latest fio versions do not provide "percentile" results if no
                    # measurements were done, so just return zeroes
                    return [0, 0, 0, 0]

            read_iops = float(data["jobs"][job_pos]["read"]["iops"])
            read_bw = float(data["jobs"][job_pos]["read"]["bw"])
            lat_key, lat_unit = get_lat_unit("lat", data["jobs"][job_pos]["read"])
            read_avg_lat = float(data["jobs"][job_pos]["read"][lat_key]["mean"])
            read_min_lat = float(data["jobs"][job_pos]["read"][lat_key]["min"])
            read_max_lat = float(data["jobs"][job_pos]["read"][lat_key]["max"])
            clat_key, clat_unit = get_lat_unit("clat", data["jobs"][job_pos]["read"])
            read_p99_lat, read_p99_9_lat, read_p99_99_lat, read_p99_999_lat = get_clat_percentiles(
                data["jobs"][job_pos]["read"][clat_key])

            if "ns" in lat_unit:
                read_avg_lat, read_min_lat, read_max_lat = [x / 1000 for x in [read_avg_lat, read_min_lat, read_max_lat]]
            if "ns" in clat_unit:
                read_p99_lat = read_p99_lat / 1000
                read_p99_9_lat = read_p99_9_lat / 1000
                read_p99_99_lat = read_p99_99_lat / 1000
                read_p99_999_lat = read_p99_999_lat / 1000

            write_iops = float(data["jobs"][job_pos]["write"]["iops"])
            write_bw = float(data["jobs"][job_pos]["write"]["bw"])
            lat_key, lat_unit = get_lat_unit("lat", data["jobs"][job_pos]["write"])
            write_avg_lat = float(data["jobs"][job_pos]["write"][lat_key]["mean"])
            write_min_lat = float(data["jobs"][job_pos]["write"][lat_key]["min"])
            write_max_lat = float(data["jobs"][job_pos]["write"][lat_key]["max"])
            clat_key, clat_unit = get_lat_unit("clat", data["jobs"][job_pos]["write"])
            write_p99_lat, write_p99_9_lat, write_p99_99_lat, write_p99_999_lat = get_clat_percentiles(
                data["jobs"][job_pos]["write"][clat_key])

            if "ns" in lat_unit:
                write_avg_lat, write_min_lat, write_max_lat = [x / 1000 for x in [write_avg_lat, write_min_lat, write_max_lat]]
            if "ns" in clat_unit:
                write_p99_lat = write_p99_lat / 1000
                write_p99_9_lat = write_p99_9_lat / 1000
                write_p99_99_lat = write_p99_99_lat / 1000
                write_p99_999_lat = write_p99_999_lat / 1000

        return [read_iops, read_bw, read_avg_lat, read_min_lat, read_max_lat,
                read_p99_lat, read_p99_9_lat, read_p99_99_lat, read_p99_999_lat,
                write_iops, write_bw, write_avg_lat, write_min_lat, write_max_lat,
                write_p99_lat, write_p99_9_lat, write_p99_99_lat, write_p99_999_lat]

    def parse_results(self, results_dir, csv_file):
        files = os.listdir(results_dir)
        fio_files = filter(lambda x: ".fio" in x, files)
        json_files = [x for x in files if ".json" in x]

        headers = ["read_iops", "read_bw", "read_avg_lat_us", "read_min_lat_us", "read_max_lat_us",
                   "read_p99_lat_us", "read_p99.9_lat_us", "read_p99.99_lat_us", "read_p99.999_lat_us",
                   "write_iops", "write_bw", "write_avg_lat_us", "write_min_lat_us", "write_max_lat_us",
                   "write_p99_lat_us", "write_p99.9_lat_us", "write_p99.99_lat_us", "write_p99.999_lat_us"]

        aggr_headers = ["iops", "bw", "avg_lat_us", "min_lat_us", "max_lat_us",
                        "p99_lat_us", "p99.9_lat_us", "p99.99_lat_us", "p99.999_lat_us"]

        header_line = ",".join(["Name", *headers])
        aggr_header_line = ",".join(["Name", *aggr_headers])

        # Create empty results file
        with open(os.path.join(results_dir, csv_file), "w") as fh:
            fh.write(aggr_header_line + "\n")
        rows = set()

        for fio_config in fio_files:
            self.log_print("Getting FIO stats for %s" % fio_config)
            job_name, _ = os.path.splitext(fio_config)

            # Look in the filename for rwmixread value. Function arguments do
            # not have that information.
            # TODO: Improve this function by directly using workload params instead
            # of regexing through filenames.
            if "read" in job_name:
                rw_mixread = 1
            elif "write" in job_name:
                rw_mixread = 0
            else:
                rw_mixread = float(re.search(r"m_(\d+)", job_name).group(1)) / 100

            # If "_CPU" exists in name - ignore it
            # Initiators for the same job could have different num_cores parameter
            job_name = re.sub(r"_\d+CPU", "", job_name)
            job_result_files = [x for x in json_files if x.startswith(job_name)]
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
                i_results_filename = re.sub(r"run_\d+_", "", i_results[0].replace("json", "csv"))

                separate_stats = []
                for r in i_results:
                    try:
                        stats = self.read_json_stats(os.path.join(results_dir, r))
                        separate_stats.append(stats)
                        self.log_print(stats)
                    except JSONDecodeError as e:
                        self.log_print("ERROR: Failed to parse %s results! Results might be incomplete!")

                init_results = [sum(x) for x in zip(*separate_stats)]
                init_results = [x / len(separate_stats) for x in init_results]
                inits_avg_results.append(init_results)

                self.log_print("\tAverage results for initiator %s" % i)
                self.log_print(init_results)
                with open(os.path.join(results_dir, i_results_filename), "w") as fh:
                    fh.write(header_line + "\n")
                    fh.write(",".join([job_name, *["{0:.3f}".format(x) for x in init_results]]) + "\n")

            # Sum results of all initiators running this FIO job.
            # Latency results are an average of latencies from accros all initiators.
            inits_avg_results = [sum(x) for x in zip(*inits_avg_results)]
            inits_avg_results = OrderedDict(zip(headers, inits_avg_results))
            for key in inits_avg_results:
                if "lat" in key:
                    inits_avg_results[key] /= len(inits_names)

            # Aggregate separate read/write values into common labels
            # Take rw_mixread into consideration for mixed read/write workloads.
            aggregate_results = OrderedDict()
            for h in aggr_headers:
                read_stat, write_stat = [float(value) for key, value in inits_avg_results.items() if h in key]
                if "lat" in h:
                    _ = rw_mixread * read_stat + (1 - rw_mixread) * write_stat
                else:
                    _ = read_stat + write_stat
                aggregate_results[h] = "{0:.3f}".format(_)

            rows.add(",".join([job_name, *aggregate_results.values()]))

        # Save results to file
        for row in rows:
            with open(os.path.join(results_dir, csv_file), "a") as fh:
                fh.write(row + "\n")
        self.log_print("You can find the test results in the file %s" % os.path.join(results_dir, csv_file))

    def measure_sar(self, results_dir, sar_file_name):
        self.log_print("Waiting %d delay before measuring SAR stats" % self.sar_delay)
        cpu_number = os.cpu_count()
        sar_idle_sum = 0
        time.sleep(self.sar_delay)
        out = self.exec_cmd(["sar", "-P", "ALL", "%s" % self.sar_interval, "%s" % self.sar_count])
        with open(os.path.join(results_dir, sar_file_name), "w") as fh:
            for line in out.split("\n"):
                if "Average" in line:
                    if "CPU" in line:
                        self.log_print("Summary CPU utilization from SAR:")
                        self.log_print(line)
                    elif "all" in line:
                        self.log_print(line)
                    else:
                        sar_idle_sum += float(line.split()[7])
            fh.write(out)
        sar_cpu_usage = cpu_number * 100 - sar_idle_sum
        with open(os.path.join(results_dir, sar_file_name), "a") as f:
            f.write("Total CPU used: " + str(sar_cpu_usage))

    def ethtool_after_fio_ramp(self, fio_ramp_time):
        time.sleep(fio_ramp_time//2)
        nic_names = [self.get_nic_name_by_ip(n) for n in self.nic_ips]
        for nic in nic_names:
            self.log_print(nic)
            self.exec_cmd(["sudo", "ethtool", "--set-priv-flags", nic,
                           "channel-pkt-inspect-optimize", "off"])  # Disable channel packet inspection optimization

    def measure_pcm_memory(self, results_dir, pcm_file_name):
        time.sleep(self.pcm_delay)
        cmd = ["%s/pcm-memory.x" % self.pcm_dir, "%s" % self.pcm_interval, "-csv=%s/%s" % (results_dir, pcm_file_name)]
        pcm_memory = subprocess.Popen(cmd)
        time.sleep(self.pcm_count)
        pcm_memory.terminate()

    def measure_pcm(self, results_dir, pcm_file_name):
        time.sleep(self.pcm_delay)
        cmd = ["%s/pcm.x" % self.pcm_dir, "%s" % self.pcm_interval, "-i=%s" % self.pcm_count, "-csv=%s/%s" % (results_dir, pcm_file_name)]
        subprocess.run(cmd)
        df = pd.read_csv(os.path.join(results_dir, pcm_file_name), header=[0, 1])
        df = df.rename(columns=lambda x: re.sub(r'Unnamed:[\w\s]*$', '', x))
        skt = df.loc[:, df.columns.get_level_values(1).isin({'UPI0', 'UPI1', 'UPI2'})]
        skt_pcm_file_name = "_".join(["skt", pcm_file_name])
        skt.to_csv(os.path.join(results_dir, skt_pcm_file_name), index=False)

    def measure_pcm_power(self, results_dir, pcm_power_file_name):
        time.sleep(self.pcm_delay)
        out = self.exec_cmd(["%s/pcm-power.x" % self.pcm_dir, "%s" % self.pcm_interval, "-i=%s" % self.pcm_count])
        with open(os.path.join(results_dir, pcm_power_file_name), "w") as fh:
            fh.write(out)

    def measure_network_bandwidth(self, results_dir, bandwidth_file_name):
        self.log_print("INFO: starting network bandwidth measure")
        self.exec_cmd(["bwm-ng", "-o", "csv", "-F", "%s/%s" % (results_dir, bandwidth_file_name),
                       "-a", "1", "-t", "1000", "-c", str(self.bandwidth_count)])

    def measure_dpdk_memory(self, results_dir):
        self.log_print("INFO: waiting to generate DPDK memory usage")
        time.sleep(self.dpdk_wait_time)
        self.log_print("INFO: generating DPDK memory usage")
        rpc.env.env_dpdk_get_mem_stats
        os.rename("/tmp/spdk_mem_dump.txt", "%s/spdk_mem_dump.txt" % (results_dir))

    def sys_config(self):
        self.log_print("====Kernel release:====")
        self.log_print(os.uname().release)
        self.log_print("====Kernel command line:====")
        with open('/proc/cmdline') as f:
            cmdline = f.readlines()
            self.log_print('\n'.join(self.get_uncommented_lines(cmdline)))
        self.log_print("====sysctl conf:====")
        with open('/etc/sysctl.conf') as f:
            sysctl = f.readlines()
            self.log_print('\n'.join(self.get_uncommented_lines(sysctl)))
        self.log_print("====Cpu power info:====")
        self.log_print(self.exec_cmd(["cpupower", "frequency-info"]))
        self.log_print("====zcopy settings:====")
        self.log_print("zcopy enabled: %s" % (self.enable_zcopy))
        self.log_print("====Scheduler settings:====")
        self.log_print("SPDK scheduler: %s" % (self.scheduler_name))


class Initiator(Server):
    def __init__(self, name, general_config, initiator_config):
        super(Initiator, self).__init__(name, general_config, initiator_config)

        # Required fields
        self.ip = initiator_config["ip"]
        self.target_nic_ips = initiator_config["target_nic_ips"]

        # Defaults
        self.cpus_allowed = None
        self.cpus_allowed_policy = "shared"
        self.spdk_dir = "/tmp/spdk"
        self.fio_bin = "/usr/src/fio/fio"
        self.nvmecli_bin = "nvme"
        self.cpu_frequency = None
        self.subsystem_info_list = []

        if "spdk_dir" in initiator_config:
            self.spdk_dir = initiator_config["spdk_dir"]
        if "fio_bin" in initiator_config:
            self.fio_bin = initiator_config["fio_bin"]
        if "nvmecli_bin" in initiator_config:
            self.nvmecli_bin = initiator_config["nvmecli_bin"]
        if "cpus_allowed" in initiator_config:
            self.cpus_allowed = initiator_config["cpus_allowed"]
        if "cpus_allowed_policy" in initiator_config:
            self.cpus_allowed_policy = initiator_config["cpus_allowed_policy"]
        if "cpu_frequency" in initiator_config:
            self.cpu_frequency = initiator_config["cpu_frequency"]
        if os.getenv('SPDK_WORKSPACE'):
            self.spdk_dir = os.getenv('SPDK_WORKSPACE')

        self.ssh_connection = paramiko.SSHClient()
        self.ssh_connection.set_missing_host_key_policy(paramiko.AutoAddPolicy())
        self.ssh_connection.connect(self.ip, username=self.username, password=self.password)
        self.exec_cmd(["sudo", "rm", "-rf", "%s/nvmf_perf" % self.spdk_dir])
        self.exec_cmd(["mkdir", "-p", "%s" % self.spdk_dir])
        self._nics_json_obj = json.loads(self.exec_cmd(["ip", "-j", "address", "show"]))

        if "skip_spdk_install" not in general_config or general_config["skip_spdk_install"] is False:
            self.copy_spdk("/tmp/spdk.zip")
        self.set_local_nic_info(self.set_local_nic_info_helper())
        self.set_cpu_frequency()
        self.configure_system()
        if self.enable_adq:
            self.configure_adq()
        self.sys_config()

    def set_local_nic_info_helper(self):
        return json.loads(self.exec_cmd(["lshw", "-json"]))

    def stop(self):
        self.ssh_connection.close()

    def exec_cmd(self, cmd, stderr_redirect=False, change_dir=None):
        if change_dir:
            cmd = ["cd", change_dir, ";", *cmd]

        # In case one of the command elements contains whitespace and is not
        # already quoted, # (e.g. when calling sysctl) quote it again to prevent expansion
        # when sending to remote system.
        for i, c in enumerate(cmd):
            if (" " in c or "\t" in c) and not (c.startswith("'") and c.endswith("'")):
                cmd[i] = '"%s"' % c
        cmd = " ".join(cmd)

        # Redirect stderr to stdout thanks using get_pty option if needed
        _, stdout, _ = self.ssh_connection.exec_command(cmd, get_pty=stderr_redirect)
        out = stdout.read().decode(encoding="utf-8")

        # Check the return code
        rc = stdout.channel.recv_exit_status()
        if rc:
            raise CalledProcessError(int(rc), cmd, out)

        return out

    def put_file(self, local, remote_dest):
        ftp = self.ssh_connection.open_sftp()
        ftp.put(local, remote_dest)
        ftp.close()

    def get_file(self, remote, local_dest):
        ftp = self.ssh_connection.open_sftp()
        ftp.get(remote, local_dest)
        ftp.close()

    def copy_spdk(self, local_spdk_zip):
        self.log_print("Copying SPDK sources to initiator %s" % self.name)
        self.put_file(local_spdk_zip, "/tmp/spdk_drop.zip")
        self.log_print("Copied sources zip from target")
        self.exec_cmd(["unzip", "-qo", "/tmp/spdk_drop.zip", "-d", self.spdk_dir])
        self.log_print("Sources unpacked")

    def copy_result_files(self, dest_dir):
        self.log_print("Copying results")

        if not os.path.exists(dest_dir):
            os.mkdir(dest_dir)

        # Get list of result files from initiator and copy them back to target
        file_list = self.exec_cmd(["ls", "%s/nvmf_perf" % self.spdk_dir]).strip().split("\n")

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
                                 "discover", "-t", "%s" % self.transport,
                                 "-s", "%s" % (4420 + subsys_no),
                                 "-a", "%s" % ip]

            try:
                stdout = self.exec_cmd(nvme_discover_cmd)
                if stdout:
                    nvme_discover_output = nvme_discover_output + stdout
            except CalledProcessError:
                # Do nothing. In case of discovering remote subsystems of kernel target
                # we expect "nvme discover" to fail a bunch of times because we basically
                # scan ports.
                pass

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
        self.subsystem_info_list = subsystems

    def gen_fio_filename_conf(self, *args, **kwargs):
        # Logic implemented in SPDKInitiator and KernelInitiator classes
        pass

    def gen_fio_config(self, rw, rwmixread, block_size, io_depth, subsys_no, num_jobs=None, ramp_time=0, run_time=10, rate_iops=0):
        fio_conf_template = """
[global]
ioengine={ioengine}
{spdk_conf}
thread=1
group_reporting=1
direct=1
percentile_list=50:90:99:99.5:99.9:99.99:99.999

norandommap=1
rw={rw}
rwmixread={rwmixread}
bs={block_size}
time_based=1
ramp_time={ramp_time}
runtime={run_time}
rate_iops={rate_iops}
"""
        if "spdk" in self.mode:
            bdev_conf = self.gen_spdk_bdev_conf(self.subsystem_info_list)
            self.exec_cmd(["echo", "'%s'" % bdev_conf, ">", "%s/bdev.conf" % self.spdk_dir])
            ioengine = "%s/build/fio/spdk_bdev" % self.spdk_dir
            spdk_conf = "spdk_json_conf=%s/bdev.conf" % self.spdk_dir
        else:
            ioengine = self.ioengine
            spdk_conf = ""
            out = self.exec_cmd(["sudo", "nvme", "list", "|", "grep", "-E", "'SPDK|Linux'",
                                 "|", "awk", "'{print $1}'"])
            subsystems = [x for x in out.split("\n") if "nvme" in x]

        if self.cpus_allowed is not None:
            self.log_print("Limiting FIO workload execution on specific cores %s" % self.cpus_allowed)
            cpus_num = 0
            cpus = self.cpus_allowed.split(",")
            for cpu in cpus:
                if "-" in cpu:
                    a, b = cpu.split("-")
                    a = int(a)
                    b = int(b)
                    cpus_num += len(range(a, b))
                else:
                    cpus_num += 1
            self.num_cores = cpus_num
            threads = range(0, self.num_cores)
        elif hasattr(self, 'num_cores'):
            self.log_print("Limiting FIO workload execution to %s cores" % self.num_cores)
            threads = range(0, int(self.num_cores))
        else:
            self.num_cores = len(subsystems)
            threads = range(0, len(subsystems))

        if "spdk" in self.mode:
            filename_section = self.gen_fio_filename_conf(self.subsystem_info_list, threads, io_depth, num_jobs)
        else:
            filename_section = self.gen_fio_filename_conf(threads, io_depth, num_jobs)

        fio_config = fio_conf_template.format(ioengine=ioengine, spdk_conf=spdk_conf,
                                              rw=rw, rwmixread=rwmixread, block_size=block_size,
                                              ramp_time=ramp_time, run_time=run_time, rate_iops=rate_iops)

        # TODO: hipri disabled for now, as it causes fio errors:
        # io_u error on file /dev/nvme2n1: Operation not supported
        # See comment in KernelInitiator class, kernel_init_connect() function
        if hasattr(self, "ioengine") and "io_uring" in self.ioengine:
            fio_config = fio_config + """
fixedbufs=1
registerfiles=1
#hipri=1
"""
        if num_jobs:
            fio_config = fio_config + "numjobs=%s \n" % num_jobs
        if self.cpus_allowed is not None:
            fio_config = fio_config + "cpus_allowed=%s \n" % self.cpus_allowed
            fio_config = fio_config + "cpus_allowed_policy=%s \n" % self.cpus_allowed_policy
        fio_config = fio_config + filename_section

        fio_config_filename = "%s_%s_%s_m_%s" % (block_size, io_depth, rw, rwmixread)
        if hasattr(self, "num_cores"):
            fio_config_filename += "_%sCPU" % self.num_cores
        fio_config_filename += ".fio"

        self.exec_cmd(["mkdir", "-p", "%s/nvmf_perf" % self.spdk_dir])
        self.exec_cmd(["echo", "'%s'" % fio_config, ">", "%s/nvmf_perf/%s" % (self.spdk_dir, fio_config_filename)])
        self.log_print("Created FIO Config:")
        self.log_print(fio_config)

        return os.path.join(self.spdk_dir, "nvmf_perf", fio_config_filename)

    def set_cpu_frequency(self):
        if self.cpu_frequency is not None:
            try:
                self.exec_cmd(["sudo", "cpupower", "frequency-set", "-g", "userspace"], True)
                self.exec_cmd(["sudo", "cpupower", "frequency-set", "-f", "%s" % self.cpu_frequency], True)
                self.log_print(self.exec_cmd(["sudo", "cpupower", "frequency-info"]))
            except Exception:
                self.log_print("ERROR: cpu_frequency will not work when intel_pstate is enabled!")
                sys.exit()
        else:
            self.log_print("WARNING: you have disabled intel_pstate and using default cpu governance.")

    def run_fio(self, fio_config_file, run_num=None):
        job_name, _ = os.path.splitext(fio_config_file)
        self.log_print("Starting FIO run for job: %s" % job_name)
        self.log_print("Using FIO: %s" % self.fio_bin)

        if run_num:
            for i in range(1, run_num + 1):
                output_filename = job_name + "_run_" + str(i) + "_" + self.name + ".json"
                try:
                    output = self.exec_cmd(["sudo", self.fio_bin, fio_config_file, "--output-format=json",
                                            "--output=%s" % output_filename, "--eta=never"], True)
                    self.log_print(output)
                except subprocess.CalledProcessError as e:
                    self.log_print("ERROR: Fio process failed!")
                    self.log_print(e.stdout)
        else:
            output_filename = job_name + "_" + self.name + ".json"
            output = self.exec_cmd(["sudo", self.fio_bin,
                                    fio_config_file, "--output-format=json",
                                    "--output" % output_filename], True)
            self.log_print(output)
        self.log_print("FIO run finished. Results in: %s" % output_filename)

    def sys_config(self):
        self.log_print("====Kernel release:====")
        self.log_print(self.exec_cmd(["uname", "-r"]))
        self.log_print("====Kernel command line:====")
        cmdline = self.exec_cmd(["cat", "/proc/cmdline"])
        self.log_print('\n'.join(self.get_uncommented_lines(cmdline.splitlines())))
        self.log_print("====sysctl conf:====")
        sysctl = self.exec_cmd(["cat", "/etc/sysctl.conf"])
        self.log_print('\n'.join(self.get_uncommented_lines(sysctl.splitlines())))
        self.log_print("====Cpu power info:====")
        self.log_print(self.exec_cmd(["cpupower", "frequency-info"]))


class KernelTarget(Target):
    def __init__(self, name, general_config, target_config):
        super(KernelTarget, self).__init__(name, general_config, target_config)
        # Defaults
        self.nvmet_bin = "nvmetcli"

        if "nvmet_bin" in target_config:
            self.nvmet_bin = target_config["nvmet_bin"]

    def stop(self):
        nvmet_command(self.nvmet_bin, "clear")

    def kernel_tgt_gen_subsystem_conf(self, nvme_list, address_list):

        nvmet_cfg = {
            "ports": [],
            "hosts": [],
            "subsystems": [],
        }

        # Split disks between NIC IP's
        disks_per_ip = int(len(nvme_list) / len(address_list))
        disk_chunks = [nvme_list[i * disks_per_ip:disks_per_ip + disks_per_ip * i] for i in range(0, len(address_list))]

        # Add remaining drives
        for i, disk in enumerate(nvme_list[disks_per_ip * len(address_list):]):
            disks_chunks[i].append(disk)

        subsys_no = 1
        port_no = 0
        for ip, chunk in zip(address_list, disk_chunks):
            for disk in chunk:
                nqn = "nqn.2018-09.io.spdk:cnode%s" % subsys_no
                nvmet_cfg["subsystems"].append({
                    "allowed_hosts": [],
                    "attr": {
                        "allow_any_host": "1",
                        "serial": "SPDK00%s" % subsys_no,
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
                    "nqn": nqn
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
                    "subsystems": [nqn]
                })
                subsys_no += 1
                port_no += 1
                self.subsystem_info_list.append([port_no, nqn, ip])

        with open("kernel.conf", "w") as fh:
            fh.write(json.dumps(nvmet_cfg, indent=2))
        pass

    def tgt_start(self):
        self.log_print("Configuring kernel NVMeOF Target")

        if self.null_block:
            print("Configuring with null block device.")
            null_blk_list = ["/dev/nullb{}".format(x) for x in range(self.null_block)]
            self.kernel_tgt_gen_subsystem_conf(null_blk_list, self.nic_ips)
            self.subsys_no = len(null_blk_list)
        else:
            print("Configuring with NVMe drives.")
            nvme_list = get_nvme_devices()
            self.kernel_tgt_gen_subsystem_conf(nvme_list, self.nic_ips)
            self.subsys_no = len(nvme_list)

        nvmet_command(self.nvmet_bin, "clear")
        nvmet_command(self.nvmet_bin, "restore kernel.conf")

        if self.enable_adq:
            self.adq_configure_tc()

        self.log_print("Done configuring kernel NVMeOF Target")


class SPDKTarget(Target):
    def __init__(self, name, general_config, target_config):
        super(SPDKTarget, self).__init__(name, general_config, target_config)

        # Required fields
        self.core_mask = target_config["core_mask"]
        self.num_cores = self.get_num_cores(self.core_mask)

        # Defaults
        self.dif_insert_strip = False
        self.null_block_dif_type = 0
        self.num_shared_buffers = 4096
        self.bpf_proc = None
        self.bpf_scripts = []
        self.enable_idxd = False

        if "num_shared_buffers" in target_config:
            self.num_shared_buffers = target_config["num_shared_buffers"]
        if "null_block_dif_type" in target_config:
            self.null_block_dif_type = target_config["null_block_dif_type"]
        if "dif_insert_strip" in target_config:
            self.dif_insert_strip = target_config["dif_insert_strip"]
        if "bpf_scripts" in target_config:
            self.bpf_scripts = target_config["bpf_scripts"]
        if "idxd_settings" in target_config:
            self.enable_idxd = target_config["idxd_settings"]

        self.log_print("====IDXD settings:====")
        self.log_print("IDXD enabled: %s" % (self.enable_idxd))

    def get_num_cores(self, core_mask):
        if "0x" in core_mask:
            return bin(int(core_mask, 16)).count("1")
        else:
            num_cores = 0
            core_mask = core_mask.replace("[", "")
            core_mask = core_mask.replace("]", "")
            for i in core_mask.split(","):
                if "-" in i:
                    x, y = i.split("-")
                    num_cores += len(range(int(x), int(y))) + 1
                else:
                    num_cores += 1
            return num_cores

    def spdk_tgt_configure(self):
        self.log_print("Configuring SPDK NVMeOF target via RPC")

        if self.enable_adq:
            self.adq_configure_tc()

        # Create RDMA transport layer
        rpc.nvmf.nvmf_create_transport(self.client, trtype=self.transport,
                                       num_shared_buffers=self.num_shared_buffers,
                                       dif_insert_or_strip=self.dif_insert_strip,
                                       sock_priority=self.adq_priority)
        self.log_print("SPDK NVMeOF transport layer:")
        rpc.client.print_dict(rpc.nvmf.nvmf_get_transports(self.client))

        if self.null_block:
            self.spdk_tgt_add_nullblock(self.null_block)
            self.spdk_tgt_add_subsystem_conf(self.nic_ips, self.null_block)
        else:
            self.spdk_tgt_add_nvme_conf()
            self.spdk_tgt_add_subsystem_conf(self.nic_ips)

        self.log_print("Done configuring SPDK NVMeOF Target")

    def spdk_tgt_add_nullblock(self, null_block_count):
        md_size = 0
        block_size = 4096
        if self.null_block_dif_type != 0:
            md_size = 128

        self.log_print("Adding null block bdevices to config via RPC")
        for i in range(null_block_count):
            self.log_print("Setting bdev protection to :%s" % self.null_block_dif_type)
            rpc.bdev.bdev_null_create(self.client, 102400, block_size + md_size, "Nvme{}n1".format(i),
                                      dif_type=self.null_block_dif_type, md_size=md_size)
        self.log_print("SPDK Bdevs configuration:")
        rpc.client.print_dict(rpc.bdev.bdev_get_bdevs(self.client))

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
            rpc.bdev.bdev_nvme_attach_controller(self.client, name="Nvme%s" % i, trtype="PCIe", traddr=bdf)

        self.log_print("SPDK Bdevs configuration:")
        rpc.client.print_dict(rpc.bdev.bdev_get_bdevs(self.client))

    def spdk_tgt_add_subsystem_conf(self, ips=None, req_num_disks=None):
        self.log_print("Adding subsystems to config")
        port = "4420"
        if not req_num_disks:
            req_num_disks = get_nvme_devices_count()

        # Distribute bdevs between provided NICs
        num_disks = range(0, req_num_disks)
        if len(num_disks) == 1:
            disks_per_ip = 1
        else:
            disks_per_ip = int(len(num_disks) / len(ips))
        disk_chunks = [[*num_disks[i * disks_per_ip:disks_per_ip + disks_per_ip * i]] for i in range(0, len(ips))]

        # Add remaining drives
        for i, disk in enumerate(num_disks[disks_per_ip * len(ips):]):
            disk_chunks[i].append(disk)

        # Create subsystems, add bdevs to namespaces, add listeners
        for ip, chunk in zip(ips, disk_chunks):
            for c in chunk:
                nqn = "nqn.2018-09.io.spdk:cnode%s" % c
                serial = "SPDK00%s" % c
                bdev_name = "Nvme%sn1" % c
                rpc.nvmf.nvmf_create_subsystem(self.client, nqn, serial,
                                               allow_any_host=True, max_namespaces=8)
                rpc.nvmf.nvmf_subsystem_add_ns(self.client, nqn, bdev_name)

                rpc.nvmf.nvmf_subsystem_add_listener(self.client,
                                                     nqn=nqn,
                                                     trtype=self.transport,
                                                     traddr=ip,
                                                     trsvcid=port,
                                                     adrfam="ipv4")

                self.subsystem_info_list.append([port, nqn, ip])
        self.log_print("SPDK NVMeOF subsystem configuration:")
        rpc.client.print_dict(rpc.nvmf.nvmf_get_subsystems(self.client))

    def bpf_start(self):
        self.log_print("Starting BPF Trace scripts: %s" % self.bpf_scripts)
        bpf_script = os.path.join(self.spdk_dir, "scripts/bpftrace.sh")
        bpf_traces = [os.path.join(self.spdk_dir, "scripts/bpf", trace) for trace in self.bpf_scripts]
        results_path = os.path.join(self.results_dir, "bpf_traces.txt")

        with open(self.pid, "r") as fh:
            nvmf_pid = str(fh.readline())

        cmd = [bpf_script, nvmf_pid, *bpf_traces]
        self.log_print(cmd)
        self.bpf_proc = subprocess.Popen(cmd, env={"BPF_OUTFILE": results_path})

    def tgt_start(self):
        if self.null_block:
            self.subsys_no = 1
        else:
            self.subsys_no = get_nvme_devices_count()
        self.log_print("Starting SPDK NVMeOF Target process")
        nvmf_app_path = os.path.join(self.spdk_dir, "build/bin/nvmf_tgt")
        proc = subprocess.Popen([nvmf_app_path, "--wait-for-rpc", "-m", self.core_mask])
        self.pid = os.path.join(self.spdk_dir, "nvmf.pid")

        with open(self.pid, "w") as fh:
            fh.write(str(proc.pid))
        self.nvmf_proc = proc
        self.log_print("SPDK NVMeOF Target PID=%s" % self.pid)
        self.log_print("Waiting for spdk to initialize...")
        while True:
            if os.path.exists("/var/tmp/spdk.sock"):
                break
            time.sleep(1)
        self.client = rpc.client.JSONRPCClient("/var/tmp/spdk.sock")

        if self.enable_zcopy:
            rpc.sock.sock_impl_set_options(self.client, impl_name="posix",
                                           enable_zerocopy_send_server=True)
            self.log_print("Target socket options:")
            rpc.client.print_dict(rpc.sock.sock_impl_get_options(self.client, impl_name="posix"))

        if self.enable_adq:
            rpc.sock.sock_impl_set_options(self.client, impl_name="posix", enable_placement_id=1)
            rpc.bdev.bdev_nvme_set_options(self.client, timeout_us=0, action_on_timeout=None,
                                           nvme_adminq_poll_period_us=100000, retry_count=4)

        if self.enable_idxd:
            rpc.idxd.idxd_scan_accel_engine(self.client, config_number=0, config_kernel_mode=None)
            self.log_print("Target IDXD accel engine enabled")

        rpc.app.framework_set_scheduler(self.client, name=self.scheduler_name)
        rpc.framework_start_init(self.client)

        if self.bpf_scripts:
            self.bpf_start()

        self.spdk_tgt_configure()

    def stop(self):
        if self.bpf_proc:
            self.log_print("Stopping BPF Trace script")
            self.bpf_proc.terminate()
            self.bpf_proc.wait()

        if hasattr(self, "nvmf_proc"):
            try:
                self.nvmf_proc.terminate()
                self.nvmf_proc.wait()
            except Exception as e:
                self.log_print(e)
                self.nvmf_proc.kill()
                self.nvmf_proc.communicate()


class KernelInitiator(Initiator):
    def __init__(self, name, general_config, initiator_config):
        super(KernelInitiator, self).__init__(name, general_config, initiator_config)

        # Defaults
        self.extra_params = ""
        self.ioengine = "libaio"

        if "extra_params" in initiator_config:
            self.extra_params = initiator_config["extra_params"]

        if "kernel_engine" in initiator_config:
            self.ioengine = initiator_config["kernel_engine"]
            if "io_uring" in self.ioengine:
                self.extra_params = "--nr-poll-queues=8"

    def get_connected_nvme_list(self):
        json_obj = json.loads(self.exec_cmd(["sudo", "nvme", "list", "-o", "json"]))
        nvme_list = [os.path.basename(x["DevicePath"]) for x in json_obj["Devices"]
                     if "SPDK" in x["ModelNumber"] or "Linux" in x["ModelNumber"]]
        return nvme_list

    def kernel_init_connect(self):
        self.log_print("Below connection attempts may result in error messages, this is expected!")
        for subsystem in self.subsystem_info_list:
            self.log_print("Trying to connect %s %s %s" % subsystem)
            self.exec_cmd(["sudo", self.nvmecli_bin, "connect", "-t", self.transport,
                           "-s", subsystem[0], "-n", subsystem[1], "-a", subsystem[2], self.extra_params])
            time.sleep(2)

        if "io_uring" in self.ioengine:
            self.log_print("Setting block layer settings for io_uring.")

            # TODO: io_poll=1 and io_poll_delay=-1 params not set here, because
            #       apparently it's not possible for connected subsystems.
            #       Results in "error: Invalid argument"
            block_sysfs_settings = {
                "iostats": "0",
                "rq_affinity": "0",
                "nomerges": "2"
            }

            for disk in self.get_connected_nvme_list():
                sysfs = os.path.join("/sys/block", disk, "queue")
                for k, v in block_sysfs_settings.items():
                    sysfs_opt_path = os.path.join(sysfs, k)
                    try:
                        self.exec_cmd(["sudo", "bash", "-c", "echo %s > %s" % (v, sysfs_opt_path)], stderr_redirect=True)
                    except subprocess.CalledProcessError as e:
                        self.log_print("Warning: command %s failed due to error %s. %s was not set!" % (e.cmd, e.output, v))
                    finally:
                        _ = self.exec_cmd(["sudo", "cat", "%s" % (sysfs_opt_path)])
                        self.log_print("%s=%s" % (sysfs_opt_path, _))

    def kernel_init_disconnect(self):
        for subsystem in self.subsystem_info_list:
            self.exec_cmd(["sudo", self.nvmecli_bin, "disconnect", "-n", subsystem[1]])
            time.sleep(1)

    def gen_fio_filename_conf(self, threads, io_depth, num_jobs=1):
        nvme_list = [os.path.join("/dev", nvme) for nvme in self.get_connected_nvme_list()]

        filename_section = ""
        nvme_per_split = int(len(nvme_list) / len(threads))
        remainder = len(nvme_list) % len(threads)
        iterator = iter(nvme_list)
        result = []
        for i in range(len(threads)):
            result.append([])
            for _ in range(nvme_per_split):
                result[i].append(next(iterator))
                if remainder:
                    result[i].append(next(iterator))
                    remainder -= 1
        for i, r in enumerate(result):
            header = "[filename%s]" % i
            disks = "\n".join(["filename=%s" % x for x in r])
            job_section_qd = round((io_depth * len(r)) / num_jobs)
            if job_section_qd == 0:
                job_section_qd = 1
            iodepth = "iodepth=%s" % job_section_qd
            filename_section = "\n".join([filename_section, header, disks, iodepth])

        return filename_section


class SPDKInitiator(Initiator):
    def __init__(self, name, general_config, initiator_config):
        super(SPDKInitiator, self).__init__(name, general_config, initiator_config)

        if "skip_spdk_install" not in general_config or general_config["skip_spdk_install"] is False:
            self.install_spdk()

        # Required fields
        self.num_cores = initiator_config["num_cores"]

        # Optional fields
        self.enable_data_digest = False
        if "enable_data_digest" in initiator_config:
            self.enable_data_digest = initiator_config["enable_data_digest"]

    def install_spdk(self):
        self.log_print("Using fio binary %s" % self.fio_bin)
        self.exec_cmd(["git", "-C", self.spdk_dir, "submodule", "update", "--init"])
        self.exec_cmd(["git", "-C", self.spdk_dir, "clean", "-ffdx"])
        self.exec_cmd(["cd", self.spdk_dir, "&&", "./configure", "--with-rdma", "--with-fio=%s" % os.path.dirname(self.fio_bin)])
        self.exec_cmd(["make", "-C", self.spdk_dir, "clean"])
        self.exec_cmd(["make", "-C", self.spdk_dir, "-j$(($(nproc)*2))"])

        self.log_print("SPDK built")
        self.exec_cmd(["sudo", "%s/scripts/setup.sh" % self.spdk_dir])

    def gen_spdk_bdev_conf(self, remote_subsystem_list):
        bdev_cfg_section = {
            "subsystems": [
                {
                    "subsystem": "bdev",
                    "config": []
                }
            ]
        }

        for i, subsys in enumerate(remote_subsystem_list):
            sub_port, sub_nqn, sub_addr = map(lambda x: str(x), subsys)
            nvme_ctrl = {
                "method": "bdev_nvme_attach_controller",
                "params": {
                    "name": "Nvme{}".format(i),
                    "trtype": self.transport,
                    "traddr": sub_addr,
                    "trsvcid": sub_port,
                    "subnqn": sub_nqn,
                    "adrfam": "IPv4"
                }
            }

            if self.enable_adq:
                nvme_ctrl["params"].update({"priority": "1"})

            if self.enable_data_digest:
                nvme_ctrl["params"].update({"ddgst": self.enable_data_digest})

            bdev_cfg_section["subsystems"][0]["config"].append(nvme_ctrl)

        return json.dumps(bdev_cfg_section, indent=2)

    def gen_fio_filename_conf(self, subsystems, threads, io_depth, num_jobs=1):
        filename_section = ""
        if len(threads) >= len(subsystems):
            threads = range(0, len(subsystems))
        filenames = ["Nvme%sn1" % x for x in range(0, len(subsystems))]
        nvme_per_split = int(len(subsystems) / len(threads))
        remainder = len(subsystems) % len(threads)
        iterator = iter(filenames)
        result = []
        for i in range(len(threads)):
            result.append([])
            for _ in range(nvme_per_split):
                result[i].append(next(iterator))
            if remainder:
                result[i].append(next(iterator))
                remainder -= 1
        for i, r in enumerate(result):
            header = "[filename%s]" % i
            disks = "\n".join(["filename=%s" % x for x in r])
            job_section_qd = round((io_depth * len(r)) / num_jobs)
            if job_section_qd == 0:
                job_section_qd = 1
            iodepth = "iodepth=%s" % job_section_qd
            filename_section = "\n".join([filename_section, header, disks, iodepth])

        return filename_section


if __name__ == "__main__":
    script_full_dir = os.path.dirname(os.path.realpath(__file__))
    default_config_file_path = os.path.relpath(os.path.join(script_full_dir, "config.json"))

    parser = argparse.ArgumentParser(formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    parser.add_argument('-c', '--config', type=str, default=default_config_file_path,
                        help='Configuration file.')
    parser.add_argument('-r', '--results', type=str, default='/tmp/results',
                        help='Results directory.')
    parser.add_argument('-s', '--csv-filename', type=str, default='nvmf_results.csv',
                        help='CSV results filename.')

    args = parser.parse_args()

    print("Using config file: %s" % args.config)
    with open(args.config, "r") as config:
        data = json.load(config)

    initiators = []
    fio_cases = []

    general_config = data["general"]
    target_config = data["target"]
    initiator_configs = [data[x] for x in data.keys() if "initiator" in x]

    for k, v in data.items():
        if "target" in k:
            v.update({"results_dir": args.results})
            if data[k]["mode"] == "spdk":
                target_obj = SPDKTarget(k, data["general"], v)
            elif data[k]["mode"] == "kernel":
                target_obj = KernelTarget(k, data["general"], v)
                pass
        elif "initiator" in k:
            if data[k]["mode"] == "spdk":
                init_obj = SPDKInitiator(k, data["general"], v)
            elif data[k]["mode"] == "kernel":
                init_obj = KernelInitiator(k, data["general"], v)
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

            fio_rate_iops = 0
            if "rate_iops" in data[k]:
                fio_rate_iops = data[k]["rate_iops"]
        else:
            continue

    try:
        os.mkdir(args.results)
    except FileExistsError:
        pass

    # TODO: This try block is definietly too large. Need to break this up into separate
    # logical blocks to reduce size.
    try:
        target_obj.tgt_start()

        for i in initiators:
            i.discover_subsystems(i.target_nic_ips, target_obj.subsys_no)
            if i.enable_adq:
                i.adq_configure_tc()

        # Poor mans threading
        # Run FIO tests
        for block_size, io_depth, rw in fio_workloads:
            threads = []
            configs = []
            for i in initiators:
                if i.mode == "kernel":
                    i.kernel_init_connect()

                cfg = i.gen_fio_config(rw, fio_rw_mix_read, block_size, io_depth, target_obj.subsys_no,
                                       fio_num_jobs, fio_ramp_time, fio_run_time, fio_rate_iops)
                configs.append(cfg)

            for i, cfg in zip(initiators, configs):
                t = threading.Thread(target=i.run_fio, args=(cfg, fio_run_num))
                threads.append(t)
            if target_obj.enable_sar:
                sar_file_name = "_".join([str(block_size), str(rw), str(io_depth), "sar"])
                sar_file_name = ".".join([sar_file_name, "txt"])
                t = threading.Thread(target=target_obj.measure_sar, args=(args.results, sar_file_name))
                threads.append(t)

            if target_obj.enable_pcm:
                pcm_fnames = ["%s_%s_%s_%s.csv" % (block_size, rw, io_depth, x) for x in ["pcm_cpu", "pcm_memory", "pcm_power"]]

                pcm_cpu_t = threading.Thread(target=target_obj.measure_pcm, args=(args.results, pcm_fnames[0],))
                pcm_mem_t = threading.Thread(target=target_obj.measure_pcm_memory, args=(args.results, pcm_fnames[1],))
                pcm_pow_t = threading.Thread(target=target_obj.measure_pcm_power, args=(args.results, pcm_fnames[2],))

                threads.append(pcm_cpu_t)
                threads.append(pcm_mem_t)
                threads.append(pcm_pow_t)

            if target_obj.enable_bandwidth:
                bandwidth_file_name = "_".join(["bandwidth", str(block_size), str(rw), str(io_depth)])
                bandwidth_file_name = ".".join([bandwidth_file_name, "csv"])
                t = threading.Thread(target=target_obj.measure_network_bandwidth, args=(args.results, bandwidth_file_name,))
                threads.append(t)

            if target_obj.enable_dpdk_memory:
                t = threading.Thread(target=target_obj.measure_dpdk_memory, args=(args.results))
                threads.append(t)

            if target_obj.enable_adq:
                ethtool_thread = threading.Thread(target=target_obj.ethtool_after_fio_ramp, args=(fio_ramp_time,))
                threads.append(ethtool_thread)

            for t in threads:
                t.start()
            for t in threads:
                t.join()

            for i in initiators:
                if i.mode == "kernel":
                    i.kernel_init_disconnect()
                i.copy_result_files(args.results)

        target_obj.restore_governor()
        target_obj.restore_tuned()
        target_obj.restore_services()
        target_obj.restore_sysctl()
        for i in initiators:
            i.restore_governor()
            i.restore_tuned()
            i.restore_services()
            i.restore_sysctl()
        target_obj.parse_results(args.results, args.csv_filename)
    finally:
        for i in initiators:
            try:
                i.stop()
            except Exception as err:
                pass
        target_obj.stop()
