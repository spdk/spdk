import os
import re
import json
from itertools import product, chain
from subprocess import check_output, CalledProcessError, Popen


def get_used_numa_nodes():
    used_numa_nodes = set()
    for bdf in get_nvme_devices_bdf():
        with open("/sys/bus/pci/devices/%s/numa_node" % bdf, "r") as numa_file:
            output = numa_file.read()
        used_numa_nodes.add(int(output))
    return used_numa_nodes


def get_nvme_devices_count():
    output = get_nvme_devices_bdf()
    return len(output)


def get_nvme_devices_bdf():
    print("Getting BDFs for NVMe section")
    output = check_output("rootdir=$PWD; \
                          source test/common/autotest_common.sh; \
                          get_nvme_bdfs 01 08 02",
                          executable="/bin/bash", shell=True)
    output = [str(x, encoding="utf-8") for x in output.split()]
    print("Done getting BDFs")
    return output


def get_nvme_devices():
    print("Getting kernel NVMe names")
    output = check_output("lsblk -o NAME -nlp", shell=True).decode(encoding="utf-8")
    output = [x for x in output.split("\n") if "nvme" in x]
    print("Done getting kernel NVMe names")
    return output


def nvmet_command(nvmet_bin, command):
    return check_output("%s %s" % (nvmet_bin, command), shell=True).decode(encoding="utf-8")
