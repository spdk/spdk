#!/usr/bin/env python3
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2018 Intel Corporation
#  All rights reserved.
#

# Note: we use setattr
# pylint: disable=no-member

import argparse
import configparser
import inspect
import itertools
import json
import logging
import os
import re
import shutil
import subprocess
import sys
import threading
import time
import uuid
import zipfile
from abc import ABC, abstractmethod
from dataclasses import dataclass
from subprocess import CalledProcessError
from typing import Any

import pandas as pd
import paramiko

sys.path.append(os.path.dirname(__file__) + '/../../../python')

from common import parse_results
from spdk.rpc.client import JSONRPCClient
from spdk.rpc.cmd_parser import print_dict


@dataclass
class ConfigField:
    name: str = None
    default: Any = None
    required: bool = False


class Server(ABC):

    RDMA_PROTOCOL_IWARP = 0
    RDMA_PROTOCOL_ROCE = 1
    RDMA_PROTOCOL_UNKNOWN = -1

    def __init__(self, name, general_config, server_config):
        self.name = name

        self.log = logging.getLogger(self.name)

        config_fields = [
            ConfigField(name='username', required=True),
            ConfigField(name='password', required=True),
            ConfigField(name='transport', required=True),
            ConfigField(name='skip_spdk_install', default=False),
            ConfigField(name='irdma_roce_enable', default=False),
            ConfigField(name='pause_frames', default=None),
        ]

        self.read_config(config_fields, general_config)
        self.transport = self.transport.lower()

        config_fields = [
            ConfigField(name='nic_ips', required=True),
            ConfigField(name='mode', required=True),
            ConfigField(name='irq_scripts_dir', default='/usr/src/local/mlnx-tools/ofed_scripts'),
            ConfigField(name='enable_arfs', default=False),
            ConfigField(name='tuned_profile', default=''),
        ]
        self.read_config(config_fields, server_config)

        self.local_nic_info = []
        self._nics_json_obj = {}
        self.svc_restore_dict = {}
        self.sysctl_restore_dict = {}
        self.tuned_restore_dict = {}
        self.governor_restore = ""

        self.enable_adq = server_config.get('adq_enable', False)
        self.adq_priority = 1 if self.enable_adq else None
        self.irq_settings = {'mode': 'default', **server_config.get('irq_settings', {})}

        if not re.match(r'^[A-Za-z0-9\-]+$', name):
            self.log.info("Please use a name which contains only letters, numbers or dashes")
            sys.exit(1)

    def read_config(self, config_fields, config):
        for config_field in config_fields:
            value = config.get(config_field.name, config_field.default)
            if config_field.required and value is None:
                raise ValueError('%s config key is required' % config_field.name)

            setattr(self, config_field.name, value)

    @staticmethod
    def get_uncommented_lines(lines):
        return [line for line in lines if line and not line.startswith('#')]

    @staticmethod
    def get_core_list_from_mask(core_mask):
        # Generate list of individual cores from hex core mask
        # (e.g. '0xffff') or list containing:
        # - individual cores (e.g. '1, 2, 3')
        # - core ranges (e.g. '0-3')
        # - mix of both (e.g. '0, 1-4, 9, 11-13')

        core_list = []
        if "0x" in core_mask:
            core_mask_int = int(core_mask, 16)
            for i in range(core_mask_int.bit_length()):
                if (1 << i) & core_mask_int:
                    core_list.append(i)
            return core_list
        else:
            # Core list can be provided in .json config with square brackets
            # remove them first
            core_mask = core_mask.replace("[", "")
            core_mask = core_mask.replace("]", "")

            for i in core_mask.split(","):
                if "-" in i:
                    start, end = i.split("-")
                    core_range = range(int(start), int(end) + 1)
                    core_list.extend(core_range)
                else:
                    core_list.append(int(i))
            return core_list

    def get_nic_name_by_ip(self, ip):
        if not self._nics_json_obj:
            nics_json_obj = self.exec_cmd(["ip", "-j", "address", "show"])
            self._nics_json_obj = list(filter(lambda x: x["addr_info"], json.loads(nics_json_obj)))
        for nic in self._nics_json_obj:
            for addr in nic["addr_info"]:
                if ip in addr["local"]:
                    return nic["ifname"]

    @abstractmethod
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

    def get_nic_numa_node(self, nic_name):
        return int(self.exec_cmd(["cat", "/sys/class/net/%s/device/numa_node" % nic_name]))

    def get_numa_cpu_map(self):
        numa_cpu_json_obj = json.loads(self.exec_cmd(["lscpu", "-b", "-e=NODE,CPU", "-J"]))
        numa_cpu_json_map = {}

        for cpu in numa_cpu_json_obj["cpus"]:
            cpu_num = int(cpu["cpu"])
            numa_node = int(cpu["node"])
            numa_cpu_json_map.setdefault(numa_node, [])
            numa_cpu_json_map[numa_node].append(cpu_num)

        return numa_cpu_json_map

    # pylint: disable=R0201
    def exec_cmd(self, cmd, stderr_redirect=False, change_dir=None):
        return ""

    def configure_system(self):
        self.load_drivers()
        self.configure_services()
        self.configure_sysctl()
        self.configure_arfs()
        self.configure_tuned()
        self.configure_cpu_governor()
        self.configure_irq_affinity(**self.irq_settings)
        self.configure_pause_frames()

    def check_rdma_protocol(self):
        try:
            roce_ena = self.exec_cmd(["cat", "/sys/module/irdma/parameters/roce_ena"]).strip()
            return self.RDMA_PROTOCOL_IWARP if roce_ena == "0" else self.RDMA_PROTOCOL_ROCE
        except CalledProcessError as e:
            self.log.error("ERROR: failed to check RDMA protocol!")
            self.log.error("%s resulted in error: %s" % (e.cmd, e.output))
            return self.RDMA_PROTOCOL_UNKNOWN

    def load_drivers(self):
        self.log.info("Loading drivers")
        self.exec_cmd(["sudo", "modprobe", "-a", f"nvme-{self.transport}", f"nvmet-{self.transport}"])

        if self.transport != "rdma":
            return

        current_mode = self.check_rdma_protocol()
        if current_mode == self.RDMA_PROTOCOL_UNKNOWN:
            self.log.error("ERROR: failed to check RDMA protocol mode")
            return

        if self.irdma_roce_enable:
            if current_mode == self.RDMA_PROTOCOL_IWARP:
                self.reload_driver("irdma", "roce_ena=1")
                self.log.info("Loaded irdma driver with RoCE enabled")
            else:
                self.log.info("Leaving irdma driver with RoCE enabled")
        else:
            self.reload_driver("irdma", "roce_ena=0")
            self.log.info("Loaded irdma driver with iWARP enabled")

    def set_pause_frames(self, rx_state, tx_state):
        for nic_ip in self.nic_ips:
            nic_name = self.get_nic_name_by_ip(nic_ip)
            self.exec_cmd(["sudo", "ethtool", "-A", nic_name, "rx", rx_state, "tx", tx_state])

    def configure_pause_frames(self):
        if self.pause_frames is None:
            self.log.info("Keeping NIC's default pause frames setting")
            return
        elif not self.pause_frames:
            self.log.info("Turning off pause frames")
            self.set_pause_frames("off", "off")
        else:
            self.log.info("Turning on pause frames")
            self.set_pause_frames("on", "on")

    def configure_arfs(self):
        rps_flow_cnt = 512 if self.enable_arfs else 0

        nic_names = [self.get_nic_name_by_ip(n) for n in self.nic_ips]
        for nic_name in nic_names:
            rps_wildcard_path = f"/sys/class/net/{nic_name}/queues/rx-*/rps_flow_cnt"
            self.exec_cmd(["sudo", "ethtool", "-K", nic_name, "ntuple", "on"])
            self.log.info(f"Setting rps_flow_cnt={rps_flow_cnt} for {nic_name} rx queues")

            self.exec_cmd(["bash", "-c", f"echo {rps_flow_cnt} | sudo tee {rps_wildcard_path}"])

            set_values = list(set(self.exec_cmd(["sudo", "bash", "-c", f'cat {rps_wildcard_path}']).strip().split("\n")))
            if len(set_values) > 1:
                self.log.error(f"""Not all NIC {nic_name} queues had rps_flow_cnt set properly. Found rps_flow_cnt values: {set_values}""")
            elif set_values[0] != str(rps_flow_cnt):
                self.log.error(f"""Wrong rps_flow_cnt set on {nic_name} queues. Found {set_values[0]}, should be {rps_flow_cnt}""")
            else:
                self.log.info(f"Configuration of {nic_name} completed with confirmed rps_flow_cnt settings.")

        self.log.info("ARFS configuration completed.")

    def configure_adq(self):
        self.adq_load_modules()
        self.adq_configure_nic()

    def adq_load_modules(self):
        self.log.info("Modprobing ADQ-related Linux modules...")
        adq_module_deps = ["sch_mqprio", "act_mirred", "cls_flower"]
        for module in adq_module_deps:
            try:
                self.exec_cmd(["sudo", "modprobe", module])
                self.log.info("%s loaded!" % module)
            except CalledProcessError as e:
                self.log.error("ERROR: failed to load module %s" % module)
                self.log.error("%s resulted in error: %s" % (e.cmd, e.output))

    def adq_configure_tc(self):
        self.log.info("Configuring ADQ Traffic classes and filters...")

        num_queues_tc0 = 2  # 2 is minimum number of queues for TC0
        num_queues_tc1 = self.num_cores
        port_param = "dst_port" if isinstance(self, Target) else "src_port"
        xps_script_path = os.path.join(self.spdk_dir, "scripts", "perf", "nvmf", "set_xps_rxqs")

        for nic_ip in self.nic_ips:
            nic_name = self.get_nic_name_by_ip(nic_ip)
            nic_ports = [x[0] for x in self.subsystem_info_list if nic_ip in x[2]]

            tc_qdisc_map_cmd = ["sudo", "tc", "qdisc", "add", "dev", nic_name,
                                "root", "mqprio", "num_tc", "2", "map", "0", "1",
                                "queues", "%s@0" % num_queues_tc0,
                                "%s@%s" % (num_queues_tc1, num_queues_tc0),
                                "hw", "1", "mode", "channel"]
            self.log.info(" ".join(tc_qdisc_map_cmd))
            self.exec_cmd(tc_qdisc_map_cmd)

            time.sleep(5)
            tc_qdisc_ingress_cmd = ["sudo", "tc", "qdisc", "add", "dev", nic_name, "ingress"]
            self.log.info(" ".join(tc_qdisc_ingress_cmd))
            self.exec_cmd(tc_qdisc_ingress_cmd)

            nic_bdf = os.path.basename(self.exec_cmd(["readlink", "-f", "/sys/class/net/%s/device" % nic_name]).strip())
            self.exec_cmd(["sudo", "devlink", "dev", "param", "set", "pci/%s" % nic_bdf,
                           "name", "tc1_inline_fd", "value", "true", "cmode", "runtime"])

            for port in nic_ports:
                tc_filter_cmd = ["sudo", "tc", "filter", "add", "dev", nic_name,
                                 "protocol", "ip", "ingress", "prio", "1", "flower",
                                 "dst_ip", "%s/32" % nic_ip, "ip_proto", "tcp", port_param, port,
                                 "skip_sw", "hw_tc", "1"]
                self.log.info(" ".join(tc_filter_cmd))
                self.exec_cmd(tc_filter_cmd)

            # show tc configuration
            self.log.info("Show tc configuration for %s NIC..." % nic_name)
            tc_disk_out = self.exec_cmd(["sudo", "tc", "qdisc", "show", "dev", nic_name])
            tc_filter_out = self.exec_cmd(["sudo", "tc", "filter", "show", "dev", nic_name, "ingress"])
            self.log.info("%s" % tc_disk_out)
            self.log.info("%s" % tc_filter_out)

            # Ethtool coalesce settings must be applied after configuring traffic classes
            self.exec_cmd(["sudo", "ethtool", "--coalesce", nic_name, "adaptive-rx", "off", "rx-usecs", "0"])
            self.exec_cmd(["sudo", "ethtool", "--coalesce", nic_name, "adaptive-tx", "off", "tx-usecs", "500"])

            self.log.info("Running set_xps_rxqs script for %s NIC..." % nic_name)
            xps_cmd = ["sudo", xps_script_path, nic_name]
            self.log.info(xps_cmd)
            self.exec_cmd(xps_cmd)

    def reload_driver(self, driver, *modprobe_args):

        try:
            self.exec_cmd(["sudo", "rmmod", driver])
            self.exec_cmd(["sudo", "modprobe", driver, *modprobe_args])
        except CalledProcessError as e:
            self.log.error("ERROR: failed to reload %s module!" % driver)
            self.log.error("%s resulted in error: %s" % (e.cmd, e.output))

    def adq_configure_nic(self):
        self.log.info("Configuring NIC port settings for ADQ testing...")

        # Reload the driver first, to make sure any previous settings are re-set.
        self.reload_driver("ice")

        nic_names = [self.get_nic_name_by_ip(n) for n in self.nic_ips]
        for nic in nic_names:
            self.log.info(nic)
            try:
                self.exec_cmd(["sudo", "ethtool", "-K", nic,
                               "hw-tc-offload", "on"])  # Enable hardware TC offload
                self.exec_cmd(["sudo", "ethtool", "--set-priv-flags", nic, "fw-lldp-agent", "off"])  # Disable LLDP
                self.exec_cmd(["sudo", "ethtool", "--set-priv-flags", nic, "channel-pkt-inspect-optimize", "off"])
                # Following are suggested in ADQ Configuration Guide to be enabled for large block sizes,
                # but can be used with 4k block sizes as well
                self.exec_cmd(["sudo", "ethtool", "--set-priv-flags", nic, "channel-pkt-clean-bp-stop", "on"])
                self.exec_cmd(["sudo", "ethtool", "--set-priv-flags", nic, "channel-pkt-clean-bp-stop-cfg", "on"])
            except subprocess.CalledProcessError as e:
                self.log.error("ERROR: failed to configure NIC port using ethtool!")
                self.log.error("%s resulted in error: %s" % (e.cmd, e.output))
                self.log.info("Please update your NIC driver and firmware versions and try again.")
            self.log.info(self.exec_cmd(["sudo", "ethtool", "-k", nic]))
            self.log.info(self.exec_cmd(["sudo", "ethtool", "--show-priv-flags", nic]))

    def configure_services(self):
        self.log.info("Configuring active services...")
        svc_config = configparser.ConfigParser(strict=False)

        # Below list is valid only for RHEL / Fedora systems and might not
        # contain valid names for other distributions.
        svc_target_state = {
            "firewalld": "inactive",
            "irqbalance": "inactive",
            "lldpad.service": "inactive",
            "lldpad.socket": "inactive",
        }

        for service in svc_target_state:
            out = self.exec_cmd(["sudo", "systemctl", "show", "--no-page", service])
            out = "\n".join(["[%s]" % service, out])
            svc_config.read_string(out)

            if "LoadError" in svc_config[service] and "not found" in svc_config[service]["LoadError"]:
                continue

            service_state = svc_config[service]["ActiveState"]
            self.log.info("Current state of %s service is %s" % (service, service_state))
            self.svc_restore_dict.update({service: service_state})
            if service_state != "inactive":
                self.log.info("Disabling %s. It will be restored after the test has finished." % service)
                self.exec_cmd(["sudo", "systemctl", "stop", service])

    def configure_sysctl(self):
        self.log.info("Tuning sysctl settings...")

        busy_read = 0
        busy_poll = 0
        if self.enable_adq and self.mode == "spdk":
            busy_read = 1
            busy_poll = 1

        sysctl_opts = {
            "net.core.busy_poll": busy_poll,
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
            "net.core.rps_sock_flow_entries": 0,
        }

        if self.enable_adq:
            sysctl_opts.update(self.adq_set_busy_read(1))

        if self.enable_arfs:
            sysctl_opts.update({"net.core.rps_sock_flow_entries": 32768})

        for opt, value in sysctl_opts.items():
            self.sysctl_restore_dict.update({opt: self.exec_cmd(["sysctl", "-n", opt]).strip()})
            self.log.info(self.exec_cmd(["sudo", "sysctl", "-w", "%s=%s" % (opt, value)]).strip())

    def configure_tuned(self):
        if not self.tuned_profile:
            self.log.warning("WARNING: Tuned profile not set in configuration file. Skipping configuration.")
            return

        self.log.info("Configuring tuned-adm profile to %s." % self.tuned_profile)
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
                "mode": profile_mode,
            }

        self.exec_cmd(["sudo", "systemctl", "start", service])
        self.exec_cmd(["sudo", "tuned-adm", "profile", self.tuned_profile])
        self.log.info("Tuned profile set to %s." % self.exec_cmd(["cat", "/etc/tuned/active_profile"]))

    def configure_cpu_governor(self):
        self.log.info("Setting CPU governor to performance...")

        # This assumes that there is the same CPU scaling governor on each CPU
        self.governor_restore = self.exec_cmd(["cat", "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"]).strip()
        self.exec_cmd(["sudo", "cpupower", "frequency-set", "-g", "performance"])

    def configure_irq_affinity(self, mode="default", cpulist=None, exclude_cpulist=False):
        self.log.info("Setting NIC irq affinity for NICs. Using %s mode" % mode)

        if mode not in ["default", "bynode", "cpulist"]:
            raise ValueError("%s irq affinity setting not supported" % mode)

        if mode == "cpulist" and not cpulist:
            raise ValueError("%s irq affinity setting set, but no cpulist provided" % mode)

        affinity_script = "set_irq_affinity.sh"
        if "default" not in mode:
            affinity_script = "set_irq_affinity_cpulist.sh"
            system_cpu_map = self.get_numa_cpu_map()
        irq_script_path = os.path.join(self.irq_scripts_dir, affinity_script)

        def cpu_list_to_string(cpulist):
            return ",".join(map(lambda x: str(x), cpulist))

        nic_names = [self.get_nic_name_by_ip(n) for n in self.nic_ips]
        for nic_name in nic_names:
            irq_cmd = ["sudo", irq_script_path]

            # Use only CPU cores matching NIC NUMA node.
            # Remove any CPU cores if they're on exclusion list.
            if mode == "bynode":
                irq_cpus = system_cpu_map[self.get_nic_numa_node(nic_name)]
                if cpulist and exclude_cpulist:
                    disallowed_cpus = self.get_core_list_from_mask(cpulist)
                    irq_cpus = list(set(irq_cpus) - set(disallowed_cpus))
                    if not irq_cpus:
                        raise Exception("No CPUs left to process IRQs after excluding CPUs!")
                irq_cmd.append(cpu_list_to_string(irq_cpus))

            if mode == "cpulist":
                irq_cpus = self.get_core_list_from_mask(cpulist)
                if exclude_cpulist:
                    # Flatten system CPU list, we don't need NUMA awareness here
                    system_cpu_list = sorted({x for v in system_cpu_map.values() for x in v})
                    irq_cpus = list(set(system_cpu_list) - set(irq_cpus))
                    if not irq_cpus:
                        raise Exception("No CPUs left to process IRQs after excluding CPUs!")
                irq_cmd.append(cpu_list_to_string(irq_cpus))

            irq_cmd.append(nic_name)
            self.log.info(irq_cmd)
            self.exec_cmd(irq_cmd, change_dir=self.irq_scripts_dir)

    def restore_services(self):
        self.log.info("Restoring services...")
        for service, state in self.svc_restore_dict.items():
            cmd = "stop" if state == "inactive" else "start"
            self.exec_cmd(["sudo", "systemctl", cmd, service])

    def restore_sysctl(self):
        self.log.info("Restoring sysctl settings...")
        for opt, value in self.sysctl_restore_dict.items():
            self.log.info(self.exec_cmd(["sudo", "sysctl", "-w", "%s=%s" % (opt, value)]).strip())

    def restore_tuned(self):
        self.log.info("Restoring tuned-adm settings...")

        if not self.tuned_restore_dict:
            return

        if self.tuned_restore_dict["mode"] == "auto":
            self.exec_cmd(["sudo", "tuned-adm", "auto_profile"])
            self.log.info("Reverted tuned-adm to auto_profile.")
        else:
            self.exec_cmd(["sudo", "tuned-adm", "profile", self.tuned_restore_dict["profile"]])
            self.log.info("Reverted tuned-adm to %s profile." % self.tuned_restore_dict["profile"])

    def restore_governor(self):
        self.log.info("Restoring CPU governor setting...")
        if self.governor_restore:
            self.exec_cmd(["sudo", "cpupower", "frequency-set", "-g", self.governor_restore])
            self.log.info("Reverted CPU governor to %s." % self.governor_restore)

    def restore_settings(self):
        self.restore_governor()
        self.restore_tuned()
        self.restore_services()
        self.restore_sysctl()
        if self.enable_adq:
            self.reload_driver("ice")

    def check_for_throttling(self):
        patterns = [
            r"CPU\d+: Core temperature is above threshold, cpu clock is throttled",
            r"mlx5_core \S+: poll_health:\d+:\(pid \d+\): device's health compromised",
            r"mlx5_core \S+: print_health_info:\d+:\(pid \d+\): Health issue observed, High temperature",
            r"mlx5_core \S+: temp_warn:\d+:\(pid \d+\): High temperature on sensors",
        ]

        issue_found = False
        try:
            output = self.exec_cmd(["sudo", "dmesg"])

            for line in output.split("\n"):
                for pattern in patterns:
                    if re.search(pattern, line):
                        self.log.warning("Throttling or temperature issue found")
                        issue_found = True
            return 1 if issue_found else 0
        except Exception as e:
            self.log.error("ERROR: failed to execute dmesg command")
            self.log.error(f"Error {e}")
            return 1

    def stop(self):
        if self.check_for_throttling():
            err_message = (f"Throttling or temperature issue found on {self.name} system! "
                           "Check system logs!")
            raise CpuThrottlingError(err_message)


class Target(Server):
    def __init__(self, name, general_config, target_config):
        super().__init__(name, general_config, target_config)

        # Defaults
        self._nics_json_obj = json.loads(self.exec_cmd(["ip", "-j", "address", "show"]))
        self.subsystem_info_list = []
        self.initiator_info = []

        config_fields = [
            ConfigField(name='mode', required=True),
            ConfigField(name='results_dir', required=True),
            ConfigField(name='enable_pm', default=True),
            ConfigField(name='enable_sar', default=True),
            ConfigField(name='enable_pcm', default=True),
            ConfigField(name='enable_dpdk_memory', default=True),
        ]
        self.read_config(config_fields, target_config)

        self.null_block = target_config.get('null_block_devices', 0)
        self.scheduler_name = target_config.get('scheduler_settings', 'static')
        self.enable_zcopy = target_config.get('zcopy_settings', False)
        self.enable_bw = target_config.get('enable_bandwidth', True)
        self.nvme_blocklist = target_config.get('blocklist', [])
        self.nvme_allowlist = target_config.get('allowlist', [])

        # Blocklist takes precedence, remove common elements from allowlist
        self.nvme_allowlist = list(set(self.nvme_allowlist) - set(self.nvme_blocklist))

        self.log.info("Items now on allowlist: %s" % self.nvme_allowlist)
        self.log.info("Items now on blocklist: %s" % self.nvme_blocklist)

        self.script_dir = os.path.dirname(os.path.abspath(sys.argv[0]))
        self.spdk_dir = os.path.abspath(os.path.join(self.script_dir, "../../../"))
        self.set_local_nic_info(self.set_local_nic_info_helper())

        if not self.skip_spdk_install:
            self.zip_spdk_sources(self.spdk_dir, "/tmp/spdk.zip")

        self.configure_system()
        if self.enable_adq:
            self.configure_adq()
            self.configure_irq_affinity()
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
            self.log.info("Changing directory to %s" % change_dir)

        out = subprocess.check_output(cmd, stderr=stderr_opt).decode(encoding="utf-8")

        if change_dir:
            os.chdir(old_cwd)
            self.log.info("Changing directory to %s" % old_cwd)
        return out

    def zip_spdk_sources(self, spdk_dir, dest_file):
        self.log.info("Zipping SPDK source directory")
        fh = zipfile.ZipFile(dest_file, "w", zipfile.ZIP_DEFLATED)
        for root, _directories, files in os.walk(spdk_dir, followlinks=True):
            for file in files:
                fh.write(os.path.relpath(os.path.join(root, file)))
        fh.close()
        self.log.info("Done zipping")

    @staticmethod
    def _chunks(input_list, chunks_no):
        div, rem = divmod(len(input_list), chunks_no)
        for i in range(chunks_no):
            si = (div + 1) * (i if i < rem else rem) + div * (0 if i < rem else i - rem)
            yield input_list[si:si + (div + 1 if i < rem else div)]

    def spread_bdevs(self, req_disks):
        # Spread available block devices indexes:
        # - evenly across available initiator systems
        # - evenly across available NIC interfaces for
        #   each initiator
        # Not NUMA aware.
        ip_bdev_map = []
        initiator_chunks = self._chunks(range(0, req_disks), len(self.initiator_info))

        for i, (init, init_chunk) in enumerate(zip(self.initiator_info, initiator_chunks)):
            self.initiator_info[i]["bdev_range"] = init_chunk
            init_chunks_list = list(self._chunks(init_chunk, len(init["target_nic_ips"])))
            for ip, nic_chunk in zip(self.initiator_info[i]["target_nic_ips"], init_chunks_list):
                for c in nic_chunk:
                    ip_bdev_map.append((ip, c))
        return ip_bdev_map

    def measure_sar(self, results_dir, sar_file_prefix, ramp_time, run_time):
        cpu_number = os.cpu_count()
        sar_idle_sum = 0
        sar_output_file = os.path.join(results_dir, sar_file_prefix + ".txt")
        sar_cpu_util_file = os.path.join(results_dir, ".".join([sar_file_prefix + "cpu_util", "txt"]))

        self.log.info("Waiting %d seconds for ramp-up to finish before measuring SAR stats" % ramp_time)
        time.sleep(ramp_time)
        self.log.info("Starting SAR measurements")

        out = self.exec_cmd(["sar", "-P", "ALL", "%s" % 1, "%s" % run_time])
        with open(os.path.join(results_dir, sar_output_file), "w") as fh:
            for line in out.split("\n"):
                if "Average" in line:
                    if "CPU" in line:
                        self.log.info("Summary CPU utilization from SAR:")
                        self.log.info(line)
                    elif "all" in line:
                        self.log.info(line)
                    else:
                        sar_idle_sum += float(line.split()[7])
            fh.write(out)
        sar_cpu_usage = cpu_number * 100 - sar_idle_sum

        with open(os.path.join(results_dir, sar_cpu_util_file), "w") as f:
            f.write("%0.2f" % sar_cpu_usage)

    def measure_power(self, results_dir, prefix, script_full_dir, ramp_time, run_time):
        time.sleep(ramp_time)
        self.log.info("Starting power measurements")
        self.exec_cmd(["%s/../pm/collect-bmc-pm" % script_full_dir,
                      "-d", "%s" % results_dir, "-l", "-p", "%s" % prefix,
                       "-x", "-c", "%s" % run_time, "-t", "%s" % 1, "-r"])

    def measure_pcm(self, results_dir, pcm_file_name, ramp_time, run_time):
        time.sleep(ramp_time)
        cmd = ["pcm", "1", "-i=%s" % run_time,
               "-csv=%s/%s" % (results_dir, pcm_file_name)]
        out = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        df = pd.read_csv(os.path.join(results_dir, pcm_file_name), header=[0, 1])
        df = df.rename(columns=lambda x: re.sub(r'Unnamed:[\w\s]*$', '', x))
        skt = df.loc[:, df.columns.get_level_values(1).isin({'UPI0', 'UPI1', 'UPI2'})]
        skt_pcm_file_name = "_".join(["skt", pcm_file_name])
        skt.to_csv(os.path.join(results_dir, skt_pcm_file_name), index=False)

        if out.returncode:
            self.log.warning("PCM Power measurement finished with a non-zero return code.")
            self.log.warning(out.stdout.decode(encoding="utf-8"))

    def measure_network_bandwidth(self, results_dir, bandwidth_file_name, ramp_time, run_time):
        self.log.info("Waiting %d seconds for ramp-up to finish before measuring bandwidth stats" % ramp_time)
        time.sleep(ramp_time)
        self.log.info("INFO: starting network bandwidth measure")
        self.exec_cmd(["bwm-ng", "-o", "csv", "-F", "%s/%s" % (results_dir, bandwidth_file_name),
                       "-a", "1", "-t", "1000", "-c", "%s" % run_time])
        # TODO: Above command results in a .csv file containing measurements for all gathered samples.
        #       Improve this so that additional file containing measurements average is generated too.
        # TODO: Monitor only these interfaces which are currently used to run the workload.

    def measure_dpdk_memory(self, results_dir, dump_file_name, ramp_time):
        self.log.info("INFO: waiting to generate DPDK memory usage")
        time.sleep(ramp_time)
        self.log.info("INFO: generating DPDK memory usage")
        tmp_dump_file = self.client.env_dpdk_get_mem_stats()["filename"]
        shutil.move(tmp_dump_file, os.path.join(results_dir, dump_file_name))

    def sys_config(self):
        self.log.info("====Kernel release:====")
        self.log.info(os.uname().release)
        self.log.info("====Kernel command line:====")
        with open('/proc/cmdline') as f:
            cmdline = f.readlines()
            self.log.info('\n'.join(self.get_uncommented_lines(cmdline)))
        self.log.info("====sysctl conf:====")
        with open('/etc/sysctl.conf') as f:
            sysctl = f.readlines()
            self.log.info('\n'.join(self.get_uncommented_lines(sysctl)))
        self.log.info("====Cpu power info:====")
        self.log.info(self.exec_cmd(["cpupower", "frequency-info"]))
        self.log.info("====zcopy settings:====")
        self.log.info("zcopy enabled: %s" % (self.enable_zcopy))
        self.log.info("====Scheduler settings:====")
        self.log.info("SPDK scheduler: %s" % (self.scheduler_name))


class Initiator(Server):
    def __init__(self, name, general_config, initiator_config):
        super().__init__(name, general_config, initiator_config)

        # required fields and defaults
        config_fields = [
            ConfigField(name='mode', required=True),
            ConfigField(name='ip', required=True),
            ConfigField(name='target_nic_ips', required=True),
            ConfigField(name='cpus_allowed', default=None),
            ConfigField(name='cpus_allowed_policy', default='shared'),
            ConfigField(name='spdk_dir', default='/tmp/spdk'),
            ConfigField(name='fio_bin', default='/usr/src/fio/fio'),
            ConfigField(name='nvmecli_bin', default='nvme'),
            ConfigField(name='cpu_frequency', default=None),
            ConfigField(name='allow_cpu_sharing', default=True),
        ]

        self.read_config(config_fields, initiator_config)

        if os.getenv('SPDK_WORKSPACE'):
            self.spdk_dir = os.getenv('SPDK_WORKSPACE')

        self.subsystem_info_list = []

        self.ssh_connection = paramiko.SSHClient()
        self.ssh_connection.set_missing_host_key_policy(paramiko.AutoAddPolicy())
        self.ssh_connection.connect(self.ip, username=self.username, password=self.password)
        self.exec_cmd(["sudo", "rm", "-rf", "%s/nvmf_perf" % self.spdk_dir])
        self.exec_cmd(["mkdir", "-p", "%s" % self.spdk_dir])
        self._nics_json_obj = json.loads(self.exec_cmd(["ip", "-j", "address", "show"]))

        if not self.skip_spdk_install:
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
        self.restore_settings()
        try:
            super().stop()
        except CpuThrottlingError as err:
            raise err
        finally:
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
        self.log.info("Copying SPDK sources to initiator %s" % self.name)
        self.put_file(local_spdk_zip, "/tmp/spdk_drop.zip")
        self.log.info("Copied sources zip from target")
        self.exec_cmd(["unzip", "-qo", "/tmp/spdk_drop.zip", "-d", self.spdk_dir])
        self.log.info("Sources unpacked")

    def copy_result_files(self, dest_dir):
        self.log.info("Copying results")

        if not os.path.exists(dest_dir):
            os.mkdir(dest_dir)

        # Get list of result files from initiator and copy them back to target
        file_list = self.exec_cmd(["ls", "%s/nvmf_perf" % self.spdk_dir]).strip().split("\n")

        for file in file_list:
            self.get_file(os.path.join(self.spdk_dir, "nvmf_perf", file),
                          os.path.join(dest_dir, file))
        self.log.info("Done copying results")

    def match_subsystems(self, target_subsystems):
        subsystems = [subsystem for subsystem in target_subsystems if subsystem[2] in self.target_nic_ips]
        if not subsystems:
            raise Exception("No matching subsystems found on target side!")
        subsystems.sort(key=lambda x: x[1])
        self.log.info("Found matching subsystems on target side:")
        for s in subsystems:
            self.log.info(s)
        self.subsystem_info_list = subsystems

    @abstractmethod
    def init_connect(self):
        pass

    @abstractmethod
    def init_disconnect(self):
        pass

    @abstractmethod
    def gen_fio_filename_conf(self, *args, **kwargs):
        # Logic implemented in SPDKInitiator and KernelInitiator classes
        pass

    def get_route_nic_numa(self, remote_nvme_ip):
        local_nvme_nic = json.loads(self.exec_cmd(["ip", "-j", "route", "get", remote_nvme_ip]))
        local_nvme_nic = local_nvme_nic[0]["dev"]
        return self.get_nic_numa_node(local_nvme_nic)

    @staticmethod
    def gen_fio_offset_section(offset_inc, num_jobs):
        offset_inc = 100 // num_jobs if offset_inc == 0 else offset_inc
        return "\n".join(["size=%s%%" % offset_inc,
                          "offset=0%",
                          "offset_increment=%s%%" % offset_inc])

    def gen_fio_numa_section(self, fio_filenames_list, num_jobs):
        numa_stats = {}
        allowed_cpus = []
        for nvme in fio_filenames_list:
            nvme_numa = self.get_nvme_subsystem_numa(os.path.basename(nvme))
            numa_stats[nvme_numa] = numa_stats.setdefault(nvme_numa, 0) + 1

        # Use the most common NUMA node for this chunk to allocate memory and CPUs
        section_local_numa = sorted(numa_stats.items(), key=lambda item: item[1], reverse=True)[0][0]

        # Check if we have enough free CPUs to pop from the list before assigning them
        if len(self.available_cpus[section_local_numa]) < num_jobs:
            if self.allow_cpu_sharing:
                self.log.info("Regenerating available CPU list %s" % section_local_numa)
                # Remove still available CPUs from the regenerated list. We don't want to
                # regenerate it with duplicates.
                cpus_regen = set(self.get_numa_cpu_map()[section_local_numa]) - set(self.available_cpus[section_local_numa])
                self.available_cpus[section_local_numa].extend(cpus_regen)
                self.log.info(self.log.info(self.available_cpus[section_local_numa]))
            else:
                self.log.error("No more free CPU cores to use from allowed_cpus list!")
                raise IndexError

        for _ in range(num_jobs):
            try:
                allowed_cpus.append(str(self.available_cpus[section_local_numa].pop(0)))
            except IndexError:
                self.log.error("No more free CPU cores to use from allowed_cpus list!")
                raise

        return "\n".join(["cpus_allowed=%s" % ",".join(allowed_cpus),
                          "numa_mem_policy=prefer:%s" % section_local_numa])

    def gen_fio_config(self, rw, block_size, io_depth, subsys_no, fio_settings):
        fio_conf_template = inspect.cleandoc("""
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
        """)

        if self.cpus_allowed is not None:
            self.log.info("Limiting FIO workload execution on specific cores %s" % self.cpus_allowed)
            self.num_cores = len(self.get_core_list_from_mask(self.cpus_allowed))
            threads = range(0, self.num_cores)
        elif hasattr(self, 'num_cores'):
            self.log.info("Limiting FIO workload execution to %s cores" % self.num_cores)
            threads = range(0, int(self.num_cores))
        else:
            self.num_cores = len(self.subsystem_info_list)
            threads = range(0, len(self.subsystem_info_list))

        filename_section = self.gen_fio_filename_conf(self.subsystem_info_list, threads, io_depth, fio_settings["num_jobs"],
                                                      fio_settings["offset"], fio_settings["offset_inc"], fio_settings["numa_align"])

        fio_config = fio_conf_template.format(ioengine=self.ioengine, spdk_conf=self.spdk_conf,
                                              rw=rw, rwmixread=fio_settings["rw_mix_read"], block_size=block_size,
                                              ramp_time=fio_settings["ramp_time"], run_time=fio_settings["run_time"],
                                              rate_iops=fio_settings["rate_iops"])

        # TODO: hipri disabled for now, as it causes fio errors:
        # io_u error on file /dev/nvme2n1: Operation not supported
        # See comment in KernelInitiator class, init_connect() function
        if "io_uring" in self.ioengine:
            fio_config = "\n".join([fio_config, "fixedbufs=1", "registerfiles=1", "#hipri=1"])
        if fio_settings["num_jobs"]:
            fio_config = "\n".join([fio_config, f"numjobs={fio_settings['num_jobs']}"])
        if self.cpus_allowed is not None:
            fio_config = "\n".join([fio_config, f"cpus_allowed={self.cpus_allowed}", f"cpus_allowed_policy={self.cpus_allowed_policy}"])
        fio_config = "\n".join([fio_config, filename_section])

        fio_config_filename = "%s_%s_%s_m_%s" % (block_size, io_depth, rw, fio_settings["rw_mix_read"])
        if hasattr(self, "num_cores"):
            fio_config_filename += "_%sCPU" % self.num_cores
        fio_config_filename += ".fio"

        self.exec_cmd(["mkdir", "-p", "%s/nvmf_perf" % self.spdk_dir])
        self.exec_cmd(["echo", "'%s'" % fio_config, ">", "%s/nvmf_perf/%s" % (self.spdk_dir, fio_config_filename)])
        self.log.info("Created FIO Config:")
        self.log.info(fio_config)

        return os.path.join(self.spdk_dir, "nvmf_perf", fio_config_filename)

    def set_cpu_frequency(self):
        if self.cpu_frequency is not None:
            try:
                self.exec_cmd(["sudo", "cpupower", "frequency-set", "-g", "userspace"], True)
                self.exec_cmd(["sudo", "cpupower", "frequency-set", "-f", "%s" % self.cpu_frequency], True)
                self.log.info(self.exec_cmd(["sudo", "cpupower", "frequency-info"]))
            except Exception:
                self.log.error("ERROR: cpu_frequency will not work when intel_pstate is enabled!")
                sys.exit(1)
        else:
            self.log.warning("WARNING: you have disabled intel_pstate and using default cpu governance.")

    def run_fio(self, fio_config_file, run_num=1):
        job_name, _ = os.path.splitext(fio_config_file)
        self.log.info("Starting FIO run for job: %s" % job_name)
        self.log.info("Using FIO: %s" % self.fio_bin)

        output_filename = job_name + "_run_" + str(run_num) + "_" + self.name + ".json"
        try:
            output = self.exec_cmd(["sudo", self.fio_bin, fio_config_file, "--output-format=json",
                                    "--output=%s" % output_filename, "--eta=never"], True)
            self.log.info(output)
            self.log.info("FIO run finished. Results in: %s" % output_filename)
        except subprocess.CalledProcessError as e:
            self.log.error("ERROR: Fio process failed!")
            self.log.error(e.stdout)

    def sys_config(self):
        self.log.info("====Kernel release:====")
        self.log.info(self.exec_cmd(["uname", "-r"]))
        self.log.info("====Kernel command line:====")
        cmdline = self.exec_cmd(["cat", "/proc/cmdline"])
        self.log.info('\n'.join(self.get_uncommented_lines(cmdline.splitlines())))
        self.log.info("====sysctl conf:====")
        sysctl = self.exec_cmd(["sudo", "cat", "/etc/sysctl.conf"])
        self.log.info('\n'.join(self.get_uncommented_lines(sysctl.splitlines())))
        self.log.info("====Cpu power info:====")
        self.log.info(self.exec_cmd(["cpupower", "frequency-info"]))


class KernelTarget(Target):
    def __init__(self, name, general_config, target_config):
        super().__init__(name, general_config, target_config)
        # Defaults
        self.nvmet_bin = target_config.get('nvmet_bin', 'nvmetcli')

    def load_drivers(self):
        self.log.info("Loading drivers")
        super().load_drivers()
        if self.null_block:
            self.exec_cmd(["sudo", "modprobe", "null_blk", "nr_devices=%s" % self.null_block])

    def configure_adq(self):
        self.log.warning("WARNING: ADQ setup not yet supported for Kernel mode. Skipping configuration.")

    def adq_configure_tc(self):
        self.log.warning("WARNING: ADQ setup not yet supported for Kernel mode. Skipping configuration.")

    def adq_set_busy_read(self, busy_read_val):
        self.log.warning("WARNING: ADQ setup not yet supported for Kernel mode. busy_read set to 0")
        return {"net.core.busy_read": 0}

    def stop(self):
        self.nvmet_command(self.nvmet_bin, "clear")
        self.restore_settings()
        super().stop()

    def get_nvme_device_bdf(self, nvme_dev_path):
        nvme_name = os.path.basename(nvme_dev_path)
        return self.exec_cmd(["cat", "/sys/block/%s/device/address" % nvme_name]).strip()

    def get_nvme_devices(self):
        dev_list = self.exec_cmd(["lsblk", "-o", "NAME", "-nlpd"]).split("\n")
        nvme_list = []
        for dev in dev_list:
            if "nvme" not in dev:
                continue
            if self.get_nvme_device_bdf(dev) in self.nvme_blocklist:
                continue
            if len(self.nvme_allowlist) == 0:
                nvme_list.append(dev)
                continue
            if self.get_nvme_device_bdf(dev) in self.nvme_allowlist:
                nvme_list.append(dev)
        return nvme_list

    def nvmet_command(self, nvmet_bin, command):
        return self.exec_cmd([nvmet_bin, *(command.split(" "))])

    def kernel_tgt_gen_subsystem_conf(self, nvme_list):

        nvmet_cfg = {
            "ports": [],
            "hosts": [],
            "subsystems": [],
        }

        for ip, bdev_num in self.spread_bdevs(len(nvme_list)):
            port = str(4420 + bdev_num)
            nqn = "nqn.2018-09.io.spdk:cnode%s" % bdev_num
            serial = "SPDK00%s" % bdev_num
            bdev_name = nvme_list[bdev_num]

            nvmet_cfg["subsystems"].append({
                "allowed_hosts": [],
                "attr": {
                    "allow_any_host": "1",
                    "serial": serial,
                    "version": "1.3",
                },
                "namespaces": [
                    {
                        "device": {
                            "path": bdev_name,
                            "uuid": "%s" % uuid.uuid4(),
                        },
                        "enable": 1,
                        "nsid": port,
                    },
                ],
                "nqn": nqn,
            })

            nvmet_cfg["ports"].append({
                "addr": {
                    "adrfam": "ipv4",
                    "traddr": ip,
                    "trsvcid": port,
                    "trtype": self.transport,
                },
                "portid": bdev_num,
                "referrals": [],
                "subsystems": [nqn],
            })

            self.subsystem_info_list.append((port, nqn, ip))
        self.subsys_no = len(self.subsystem_info_list)

        with open("kernel.conf", "w") as fh:
            fh.write(json.dumps(nvmet_cfg, indent=2))

    def tgt_start(self):
        self.log.info("Configuring kernel NVMeOF Target")

        if self.null_block:
            self.log.info("Configuring with null block device.")
            nvme_list = ["/dev/nullb{}".format(x) for x in range(self.null_block)]
        else:
            self.log.info("Configuring with NVMe drives.")
            nvme_list = self.get_nvme_devices()

        self.kernel_tgt_gen_subsystem_conf(nvme_list)
        self.subsys_no = len(nvme_list)

        self.nvmet_command(self.nvmet_bin, "clear")
        self.nvmet_command(self.nvmet_bin, "restore kernel.conf")

        if self.enable_adq:
            self.adq_configure_tc()

        self.log.info("Done configuring kernel NVMeOF Target")


class SPDKTarget(Target):
    def __init__(self, name, general_config, target_config):
        # IRQ affinity on SPDK Target side takes Target's core mask into consideration.
        # Method setting IRQ affinity is run as part of parent classes init,
        # so we need to have self.core_mask set before changing IRQ affinity.
        self.core_mask = target_config["core_mask"]
        self.num_cores = len(self.get_core_list_from_mask(self.core_mask))

        super().__init__(name, general_config, target_config)

        # Format: property, default value
        config_fields = [
            ConfigField(name='dif_insert_strip', default=False),
            ConfigField(name='null_block_dif_type', default=0),
            ConfigField(name='num_shared_buffers', default=4096),
            ConfigField(name='max_queue_depth', default=128),
            ConfigField(name='bpf_scripts', default=[]),
            ConfigField(name='scheduler_core_limit', default=None),
            ConfigField(name='dsa_settings', default=False),
            ConfigField(name='iobuf_small_pool_count', default=32767),
            ConfigField(name='iobuf_large_pool_count', default=16383),
            ConfigField(name='num_cqe', default=4096),
            ConfigField(name='sock_impl', default='posix'),
        ]

        self.read_config(config_fields, target_config)

        self.bpf_proc = None
        self.enable_dsa = False

        self.log.info("====DSA settings:====")
        self.log.info("DSA enabled: %s" % (self.enable_dsa))

    def configure_irq_affinity(self, mode="default", cpulist=None, exclude_cpulist=False):
        if mode not in ["default", "bynode", "cpulist",
                        "shared", "split", "split-bynode"]:
            self.log.error("%s irq affinity setting not supported" % mode)
            raise Exception

        # Create core list from SPDK's mask and change it to string.
        # This is the type configure_irq_affinity expects for cpulist parameter.
        spdk_tgt_core_list = self.get_core_list_from_mask(self.core_mask)
        spdk_tgt_core_list = ",".join(map(lambda x: str(x), spdk_tgt_core_list))
        spdk_tgt_core_list = "[" + spdk_tgt_core_list + "]"

        if mode == "shared":
            super().configure_irq_affinity(mode="cpulist", cpulist=spdk_tgt_core_list)
        elif mode == "split":
            super().configure_irq_affinity(mode="cpulist", cpulist=spdk_tgt_core_list, exclude_cpulist=True)
        elif mode == "split-bynode":
            super().configure_irq_affinity(mode="bynode", cpulist=spdk_tgt_core_list, exclude_cpulist=True)
        else:
            super().configure_irq_affinity(mode=mode, cpulist=cpulist, exclude_cpulist=exclude_cpulist)

    def adq_set_busy_read(self, busy_read_val):
        return {"net.core.busy_read": busy_read_val}

    def get_nvme_devices_count(self):
        return len(self.get_nvme_devices())

    def get_nvme_devices(self):
        bdev_subsys_json_obj = json.loads(self.exec_cmd([os.path.join(self.spdk_dir, "scripts/gen_nvme.sh")]))
        bdev_bdfs = []
        for bdev in bdev_subsys_json_obj["config"]:
            bdev_traddr = bdev["params"]["traddr"]
            if bdev_traddr in self.nvme_blocklist:
                continue
            if len(self.nvme_allowlist) == 0:
                bdev_bdfs.append(bdev_traddr)
            if bdev_traddr in self.nvme_allowlist:
                bdev_bdfs.append(bdev_traddr)
        return bdev_bdfs

    def spdk_tgt_configure(self):
        self.log.info("Configuring SPDK NVMeOF target via RPC")

        # Create transport layer
        nvmf_transport_params = {
            "trtype": self.transport,
            "num_shared_buffers": self.num_shared_buffers,
            "max_queue_depth": self.max_queue_depth,
            "dif_insert_or_strip": self.dif_insert_strip,
            "sock_priority": self.adq_priority,
            "num_cqe": self.num_cqe,
        }

        if self.enable_adq:
            nvmf_transport_params["acceptor_poll_rate"] = 10000

        self.client.nvmf_create_transport(**nvmf_transport_params)
        self.log.info("SPDK NVMeOF transport layer:")
        print_dict(self.client.nvmf_get_transports())

        if self.null_block:
            self.spdk_tgt_add_nullblock(self.null_block)
            self.spdk_tgt_add_subsystem_conf(self.nic_ips, self.null_block)
        else:
            self.spdk_tgt_add_nvme_conf()
            self.spdk_tgt_add_subsystem_conf(self.nic_ips)

        if self.enable_adq:
            self.adq_configure_tc()

        self.log.info("Done configuring SPDK NVMeOF Target")

    def spdk_tgt_add_nullblock(self, null_block_count):
        md_size = 0
        block_size = 4096
        if self.null_block_dif_type != 0:
            md_size = 128

        self.log.info("Adding null block bdevices to config via RPC")
        for i in range(null_block_count):
            self.log.info("Setting bdev protection to :%s" % self.null_block_dif_type)
            self.client.bdev_null_create(num_blocks=102400, block_size=block_size, name="Nvme{}n1".format(i),
                                         dif_type=self.null_block_dif_type, md_size=md_size)

        self.log.info("SPDK Bdevs configuration:")
        print_dict(self.client.bdev_get_bdevs())

    def spdk_tgt_add_nvme_conf(self, req_num_disks=None):
        self.log.info("Adding NVMe bdevs to config via RPC")

        bdfs = self.get_nvme_devices()
        bdfs = [b.replace(":", ".") for b in bdfs]

        if req_num_disks:
            if req_num_disks > len(bdfs):
                self.log.error("ERROR: Requested number of disks is more than available %s" % len(bdfs))
                sys.exit(1)
            else:
                bdfs = bdfs[0:req_num_disks]

        for i, bdf in enumerate(bdfs):
            self.client.bdev_nvme_attach_controller(name="Nvme%s" % i, trtype="PCIe", traddr=bdf)

        self.log.info("SPDK Bdevs configuration:")
        print_dict(self.client.bdev_get_bdevs())

    def spdk_tgt_add_subsystem_conf(self, ips=None, req_num_disks=None):
        self.log.info("Adding subsystems to config")
        if not req_num_disks:
            req_num_disks = self.get_nvme_devices_count()

        for ip, bdev_num in self.spread_bdevs(req_num_disks):
            port = str(4420 + bdev_num)
            nqn = "nqn.2018-09.io.spdk:cnode%s" % bdev_num
            serial = "SPDK00%s" % bdev_num
            bdev_name = "Nvme%sn1" % bdev_num

            self.client.nvmf_create_subsystem(nqn=nqn, serial_number=serial,
                                              allow_any_host=True, max_namespaces=8)
            self.client.nvmf_subsystem_add_ns(nqn=nqn, namespace=dict(bdev_name=bdev_name))
            for nqn_name in [nqn, "nqn.2014-08.org.nvmexpress.discovery"]:
                self.client.nvmf_subsystem_add_listener(nqn=nqn_name,
                                                        listen_address=dict(
                                                            trtype=self.transport,
                                                            traddr=ip,
                                                            trsvcid=port,
                                                            adrfam="ipv4"),
                                                        )
            self.subsystem_info_list.append((port, nqn, ip))
        self.subsys_no = len(self.subsystem_info_list)

        self.log.info("SPDK NVMeOF subsystem configuration:")
        print_dict(self.client.nvmf_get_subsystems())

    def bpf_start(self):
        self.log.info("Starting BPF Trace scripts: %s" % self.bpf_scripts)
        bpf_script = os.path.join(self.spdk_dir, "scripts/bpftrace.sh")
        bpf_traces = [os.path.join(self.spdk_dir, "scripts/bpf", trace) for trace in self.bpf_scripts]
        results_path = os.path.join(self.results_dir, "bpf_traces.txt")

        with open(self.pid, "r") as fh:
            nvmf_pid = str(fh.readline())

        cmd = [bpf_script, nvmf_pid, *bpf_traces]
        self.log.info(cmd)
        self.bpf_proc = subprocess.Popen(cmd, env={"BPF_OUTFILE": results_path})

    def tgt_start(self):
        if self.null_block:
            self.subsys_no = 1
        else:
            self.subsys_no = self.get_nvme_devices_count()
        self.log.info("Starting SPDK NVMeOF Target process")
        nvmf_app_path = os.path.join(self.spdk_dir, "build/bin/nvmf_tgt")
        proc = subprocess.Popen([nvmf_app_path, "--wait-for-rpc", "-m", self.core_mask])
        self.pid = os.path.join(self.spdk_dir, "nvmf.pid")

        with open(self.pid, "w") as fh:
            fh.write(str(proc.pid))
        self.nvmf_proc = proc
        self.log.info("SPDK NVMeOF Target PID=%s" % self.pid)
        self.log.info("Waiting for spdk to initialize...")
        while True:
            if os.path.exists("/var/tmp/spdk.sock"):
                break
            time.sleep(1)
        self.client = JSONRPCClient("/var/tmp/spdk.sock")

        self.client.sock_set_default_impl(impl_name=self.sock_impl)
        self.client.iobuf_set_options(small_pool_count=self.iobuf_small_pool_count,
                                      large_pool_count=self.iobuf_large_pool_count,
                                      small_bufsize=None,
                                      large_bufsize=None)

        if self.enable_zcopy:
            self.client.sock_impl_set_options(impl_name=self.sock_impl,
                                              enable_zerocopy_send_server=True)
            self.log.info("Target socket options:")
            print_dict(self.client.sock_impl_get_options(impl_name=self.sock_impl))

        if self.enable_adq:
            self.client.sock_impl_set_options(impl_name=self.sock_impl, enable_placement_id=1)
            self.client.bdev_nvme_set_options(timeout_us=0, action_on_timeout=None,
                                              nvme_adminq_poll_period_us=100000, retry_count=4)

        if self.enable_dsa:
            self.client.dsa_scan_accel_module(config_kernel_mode=None)
            self.log.info("Target DSA accel module enabled")

        self.client.framework_set_scheduler(name=self.scheduler_name, core_limit=self.scheduler_core_limit)
        self.client.framework_start_init()

        if self.bpf_scripts:
            self.bpf_start()

        self.spdk_tgt_configure()

    def stop(self):
        if self.bpf_proc:
            self.log.info("Stopping BPF Trace script")
            self.bpf_proc.terminate()
            self.bpf_proc.wait()

        if hasattr(self, "nvmf_proc"):
            try:
                self.nvmf_proc.terminate()
                self.nvmf_proc.wait(timeout=30)
            except Exception as e:
                self.log.info("Failed to terminate SPDK Target process. Sending SIGKILL.")
                self.log.info(e)
                self.nvmf_proc.kill()
                self.nvmf_proc.communicate()
                # Try to clean up RPC socket files if they were not removed
                # because of using 'kill'
                try:
                    os.remove("/var/tmp/spdk.sock")
                    os.remove("/var/tmp/spdk.sock.lock")
                except FileNotFoundError:
                    pass
        self.restore_settings()
        super().stop()


class KernelInitiator(Initiator):
    def __init__(self, name, general_config, initiator_config):
        super().__init__(name, general_config, initiator_config)

        # Defaults
        self.extra_params = initiator_config.get('extra_params', '')

        self.ioengine = "libaio"
        self.spdk_conf = ""

        if "num_cores" in initiator_config:
            self.num_cores = initiator_config["num_cores"]

        if "kernel_engine" in initiator_config:
            self.ioengine = initiator_config["kernel_engine"]
            if "io_uring" in self.ioengine:
                self.extra_params += ' --nr-poll-queues=8'

    def configure_adq(self):
        self.log.warning("WARNING: ADQ setup not yet supported for Kernel mode. Skipping configuration.")

    def adq_configure_tc(self):
        self.log.warning("WARNING: ADQ setup not yet supported for Kernel mode. Skipping configuration.")

    def adq_set_busy_read(self, busy_read_val):
        self.log.warning("WARNING: ADQ setup not yet supported for Kernel mode. busy_read set to 0")
        return {"net.core.busy_read": 0}

    def get_connected_nvme_list(self):
        json_obj = json.loads(self.exec_cmd(["sudo", "nvme", "list", "-o", "json"]))
        nvme_list = [os.path.basename(x["DevicePath"]) for x in json_obj["Devices"]
                     if "SPDK" in x["ModelNumber"] or "Linux" in x["ModelNumber"]]
        return nvme_list

    def init_connect(self):
        self.log.info("Below connection attempts may result in error messages, this is expected!")
        for subsystem in self.subsystem_info_list:
            self.log.info("Trying to connect %s %s %s" % subsystem)
            self.exec_cmd(["sudo", self.nvmecli_bin, "connect", "-t", self.transport,
                           "-s", subsystem[0], "-n", subsystem[1], "-a", subsystem[2], self.extra_params])
            time.sleep(2)

        if "io_uring" in self.ioengine:
            self.log.info("Setting block layer settings for io_uring.")

            # TODO: io_poll=1 and io_poll_delay=-1 params not set here, because
            #       apparently it's not possible for connected subsystems.
            #       Results in "error: Invalid argument"
            block_sysfs_settings = {
                "iostats": "0",
                "rq_affinity": "0",
                "nomerges": "2",
            }

            for disk in self.get_connected_nvme_list():
                sysfs = os.path.join("/sys/block", disk, "queue")
                for k, v in block_sysfs_settings.items():
                    sysfs_opt_path = os.path.join(sysfs, k)
                    try:
                        self.exec_cmd(["sudo", "bash", "-c", "echo %s > %s" % (v, sysfs_opt_path)], stderr_redirect=True)
                    except CalledProcessError as e:
                        self.log.warning("Warning: command %s failed due to error %s. %s was not set!" % (e.cmd, e.output, v))
                    finally:
                        _ = self.exec_cmd(["sudo", "cat", "%s" % (sysfs_opt_path)])
                        self.log.info("%s=%s" % (sysfs_opt_path, _))

    def init_disconnect(self):
        for subsystem in self.subsystem_info_list:
            self.exec_cmd(["sudo", self.nvmecli_bin, "disconnect", "-n", subsystem[1]])
            time.sleep(1)

    def get_nvme_subsystem_numa(self, dev_name):
        # Remove two last characters to get controller name instead of subsystem name
        nvme_ctrl = os.path.basename(dev_name)[:-2]
        remote_nvme_ip = re.search(r'(\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3})',
                                   self.exec_cmd(["cat", "/sys/class/nvme/%s/address" % nvme_ctrl]))
        return self.get_route_nic_numa(remote_nvme_ip.group(0))

    def gen_fio_filename_conf(self, subsystems, threads, io_depth, num_jobs=1, offset=False, offset_inc=0, numa_align=True):
        self.available_cpus = self.get_numa_cpu_map()
        if len(threads) >= len(subsystems):
            threads = range(0, len(subsystems))

        # Generate connected nvme devices names and sort them by used NIC numa node
        # to allow better grouping when splitting into fio sections.
        nvme_list = [os.path.join("/dev", nvme) for nvme in self.get_connected_nvme_list()]
        nvme_numas = [self.get_nvme_subsystem_numa(x) for x in nvme_list]
        nvme_list = [x for _, x in sorted(zip(nvme_numas, nvme_list))]

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

            offset_section = ""
            if offset:
                offset_section = self.gen_fio_offset_section(offset_inc, num_jobs)

            numa_opts = ""
            if numa_align:
                numa_opts = self.gen_fio_numa_section(r, num_jobs)

            filename_section = "\n".join([filename_section, header, disks, iodepth, numa_opts, offset_section, ""])

        return filename_section


class SPDKInitiator(Initiator):
    def __init__(self, name, general_config, initiator_config):
        super().__init__(name, general_config, initiator_config)

        if not self.skip_spdk_install:
            self.install_spdk()

        # Optional fields
        self.enable_data_digest = initiator_config.get('enable_data_digest', False)
        self.small_pool_count = initiator_config.get('small_pool_count', 32768)
        self.large_pool_count = initiator_config.get('large_pool_count', 16384)
        self.sock_impl = initiator_config.get('sock_impl', 'posix')

        if "num_cores" in initiator_config:
            self.num_cores = initiator_config["num_cores"]

        self.ioengine = "%s/build/fio/spdk_bdev" % self.spdk_dir
        self.spdk_conf = "spdk_json_conf=%s/bdev.conf" % self.spdk_dir

    def adq_set_busy_read(self, busy_read_val):
        return {"net.core.busy_read": busy_read_val}

    def install_spdk(self):
        self.log.info("Using fio binary %s" % self.fio_bin)
        self.exec_cmd(["git", "-C", self.spdk_dir, "submodule", "update", "--init"])
        self.exec_cmd(["git", "-C", self.spdk_dir, "clean", "-ffdx"])
        self.exec_cmd(["cd", self.spdk_dir, "&&", "./configure", "--with-rdma",
                       "--with-fio=%s" % os.path.dirname(self.fio_bin),
                       "--enable-lto", "--disable-unit-tests"])
        self.exec_cmd(["make", "-C", self.spdk_dir, "clean"])
        self.exec_cmd(["make", "-C", self.spdk_dir, "-j$(($(nproc)*2))"])

        self.log.info("SPDK built")
        self.exec_cmd(["sudo", "%s/scripts/setup.sh" % self.spdk_dir])

    def init_connect(self):
        # Not a real "connect" like when doing "nvme connect" because SPDK's fio
        # bdev plugin initiates connection just before starting IO traffic.
        # This is just to have a "init_connect" equivalent of the same function
        # from KernelInitiator class.
        # Just prepare bdev.conf JSON file for later use and consider it
        # "making a connection".
        bdev_conf = self.gen_spdk_bdev_conf(self.subsystem_info_list)
        self.exec_cmd(["echo", "'%s'" % bdev_conf, ">", "%s/bdev.conf" % self.spdk_dir])

    def init_disconnect(self):
        # SPDK Initiator does not need to explicitly disconnect as this gets done
        # after fio bdev plugin finishes IO.
        return

    def gen_spdk_bdev_conf(self, remote_subsystem_list):
        spdk_cfg_section = {
            "subsystems": [
                {
                    "subsystem": "bdev",
                    "config": [],
                },
                {
                    "subsystem": "iobuf",
                    "config": [
                        {
                            "method": "iobuf_set_options",
                            "params": {
                                "small_pool_count": self.small_pool_count,
                                "large_pool_count": self.large_pool_count,
                            },
                        },
                    ],
                },
                {
                    "subsystem": "sock",
                    "config": [
                        {
                            "method": "sock_set_default_impl",
                            "params": {
                                "impl_name": self.sock_impl,
                            },
                        },
                    ],
                },
            ],
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
                    "adrfam": "IPv4",
                },
            }

            if self.enable_adq:
                nvme_ctrl["params"].update({"priority": "1"})

            if self.enable_data_digest:
                nvme_ctrl["params"].update({"ddgst": self.enable_data_digest})

            spdk_cfg_section["subsystems"][0]["config"].append(nvme_ctrl)

        return json.dumps(spdk_cfg_section, indent=2)

    def gen_fio_filename_conf(self, subsystems, threads, io_depth, num_jobs=1, offset=False, offset_inc=0, numa_align=True):
        self.available_cpus = self.get_numa_cpu_map()
        filename_section = ""
        if len(threads) >= len(subsystems):
            threads = range(0, len(subsystems))

        # Generate expected NVMe Bdev names and sort them by used NIC numa node
        # to allow better grouping when splitting into fio sections.
        filenames = ["Nvme%sn1" % x for x in range(0, len(subsystems))]
        filename_numas = [self.get_nvme_subsystem_numa(x) for x in filenames]
        filenames = [x for _, x in sorted(zip(filename_numas, filenames))]

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

            offset_section = ""
            if offset:
                offset_section = self.gen_fio_offset_section(offset_inc, num_jobs)

            numa_opts = ""
            if numa_align:
                numa_opts = self.gen_fio_numa_section(r, num_jobs)

            filename_section = "\n".join([filename_section, header, disks, iodepth, numa_opts, offset_section, ""])

        return filename_section

    def get_nvme_subsystem_numa(self, bdev_name):
        bdev_conf_json_obj = json.loads(self.exec_cmd(["cat", "%s/bdev.conf" % self.spdk_dir]))
        bdev_conf_json_obj = bdev_conf_json_obj["subsystems"][0]["config"]

        # Remove two last characters to get controller name instead of subsystem name
        nvme_ctrl = bdev_name[:-2]
        for bdev in bdev_conf_json_obj:
            if bdev["method"] == "bdev_nvme_attach_controller" and bdev["params"]["name"] == nvme_ctrl:
                return self.get_route_nic_numa(bdev["params"]["traddr"])
        return None


def initiators_match_subsystems(initiators, target_obj):
    for i in initiators:
        i.match_subsystems(target_obj.subsystem_info_list)
        if i.enable_adq:
            i.adq_configure_tc()


def run_single_fio_test(args, initiators, configs, target_obj, block_size, io_depth, rw, fio_settings):

    for run_no in range(1, fio_settings["run_num"]+1):
        threads = []
        power_daemon = None
        measurements_prefix = "%s_%s_%s_m_%s_run_%s" % (block_size, io_depth, rw, fio_settings["rw_mix_read"], run_no)

        for i, cfg in zip(initiators, configs):
            t = threading.Thread(target=i.run_fio, args=(cfg, run_no))
            threads.append(t)
        if target_obj.enable_sar:
            sar_file_prefix = measurements_prefix + "_sar"
            t = threading.Thread(target=target_obj.measure_sar,
                                 args=(args.results, sar_file_prefix, fio_settings["ramp_time"], fio_settings["run_time"]))
            threads.append(t)

        if target_obj.enable_pcm:
            pcm_fnames = ["%s_%s.csv" % (measurements_prefix, x) for x in ["pcm_cpu"]]
            pcm_cpu_t = threading.Thread(target=target_obj.measure_pcm,
                                         args=(args.results, pcm_fnames[0], fio_settings["ramp_time"], fio_settings["run_time"]))
            threads.append(pcm_cpu_t)

        if target_obj.enable_bw:
            bandwidth_file_name = measurements_prefix + "_bandwidth.csv"
            t = threading.Thread(target=target_obj.measure_network_bandwidth,
                                 args=(args.results, bandwidth_file_name, fio_settings["ramp_time"], fio_settings["run_time"]))
            threads.append(t)

        if target_obj.enable_dpdk_memory:
            dpdk_mem_file_name = measurements_prefix + "_dpdk_mem.txt"
            t = threading.Thread(target=target_obj.measure_dpdk_memory, args=(args.results, dpdk_mem_file_name, fio_settings["ramp_time"]))
            threads.append(t)

        if target_obj.enable_pm:
            power_daemon = threading.Thread(target=target_obj.measure_power,
                                            args=(args.results, measurements_prefix, script_full_dir,
                                                  fio_settings["ramp_time"], fio_settings["run_time"]))
            threads.append(power_daemon)

        for t in threads:
            t.start()
        for t in threads:
            t.join()


def run_fio_tests(args, initiators, target_obj, fio_settings):

    for block_size, io_depth, rw in fio_settings["workloads"]:
        configs = []
        for i in initiators:
            i.init_connect()
            cfg = i.gen_fio_config(rw, block_size, io_depth, target_obj.subsys_no, fio_settings)
            configs.append(cfg)

        run_single_fio_test(args, initiators, configs, target_obj, block_size, io_depth, rw, fio_settings)

        for i in initiators:
            i.init_disconnect()
            i.copy_result_files(args.results)

    try:
        parse_results(args.results, args.csv_filename)
    except Exception as err:
        logging.error("There was an error with parsing the results")
        raise err


def validate_allowlist_settings(data, is_forced_used):
    if data["target"].get("null_block_devices"):
        logging.info("Null block devices used for testing. Allow and block lists will not be checked.")
        return True

    if is_forced_used:
        logging.warning("""Force mode used. All available NVMe drives on your system will be used,
                     which may potentially lead to data loss""")
        return True

    if not (data["target"].get("allowlist") or data["target"].get("blocklist")):
        logging.error("""This script requires allowlist or blocklist to be defined.
                      You can choose to use all available NVMe drives on your system (-f option),
                      which may potentially lead to data loss.""")
        return False

    return True


class CpuThrottlingError(Exception):
    def __init__(self, message):
        super().__init__(message)


if __name__ == "__main__":
    exit_code = 0

    script_full_dir = os.path.dirname(os.path.realpath(__file__))
    default_config_file_path = os.path.relpath(os.path.join(script_full_dir, "config.json"))

    parser = argparse.ArgumentParser(formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    parser.add_argument('-c', '--config', type=str, default=default_config_file_path,
                        help='Configuration file.')
    parser.add_argument('-r', '--results', type=str, default='/tmp/results',
                        help='Results directory.')
    parser.add_argument('-s', '--csv-filename', type=str, default='nvmf_results.csv',
                        help='CSV results filename.')
    parser.add_argument('-f', '--force', default=False, action='store_true',
                        dest='force', help="""Force script to continue and try to use all
                        available NVMe devices during test.
                        WARNING: Might result in data loss on used NVMe drives""")

    args = parser.parse_args()

    logging.basicConfig(level=logging.INFO,
                        format='[%(name)s:%(funcName)s:%(lineno)d] %(message)s')

    logging.info("Using config file: %s" % args.config)
    with open(args.config, "r") as config:
        data = json.load(config)

    initiators = []
    general_config = data["general"]
    target_config = data["target"]
    initiator_configs = [data[x] for x in data.keys() if "initiator" in x]

    if not validate_allowlist_settings(data, args.force):
        exit(1)

    for k, v in data.items():
        if "target" in k:
            v.update({"results_dir": args.results})
            if data[k]["mode"] == "spdk":
                target_obj = SPDKTarget(k, data["general"], v)
            elif data[k]["mode"] == "kernel":
                target_obj = KernelTarget(k, data["general"], v)
        elif "initiator" in k:
            if data[k]["mode"] == "spdk":
                init_obj = SPDKInitiator(k, data["general"], v)
            elif data[k]["mode"] == "kernel":
                init_obj = KernelInitiator(k, data["general"], v)
            initiators.append(init_obj)
        elif "fio" in k:
            fio_settings = {
                "workloads": itertools.product(data[k]["bs"], data[k]["qd"],  data[k]["rw"]),
                "run_time": data[k].get("run_time", 10),
                "ramp_time": data[k].get("ramp_time", 0),
                "rw_mix_read": data[k].get("rwmixread", 70),
                "run_num": data[k].get("run_num", None),
                "num_jobs": data[k].get("num_jobs", None),
                "rate_iops": data[k].get("rate_iops", 0),
                "offset": data[k].get("offset", False),
                "offset_inc": data[k].get("offset_inc", 0),
                "numa_align": data[k].get("numa_align", 1),
            }
        else:
            continue

    try:
        os.mkdir(args.results)
    except FileExistsError:
        pass

    for i in initiators:
        target_obj.initiator_info.append(
            {"name": i.name, "target_nic_ips": i.target_nic_ips, "initiator_nic_ips": i.nic_ips},
        )

    try:
        target_obj.tgt_start()
        initiators_match_subsystems(initiators, target_obj)

        # Poor mans threading
        # Run FIO tests
        run_fio_tests(args, initiators, target_obj, fio_settings)

    except Exception as e:
        logging.error("Exception occurred while running FIO tests")
        logging.error(e)
        exit_code = 1

    finally:
        for i in [*initiators, target_obj]:
            try:
                i.stop()
            except CpuThrottlingError as err:
                logging.error(err)
                exit_code = 1
            except Exception:
                pass
        sys.exit(exit_code)
