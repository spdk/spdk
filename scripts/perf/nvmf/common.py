import os
import re
import json
from itertools import product, chain
from subprocess import check_output, Popen


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
    output = check_output("source scripts/common.sh; iter_pci_class_code 01 08 02",
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


def parse_results(output_dir="."):
    json_files = os.listdir(output_dir)
    json_files = sorted(json_files)
    json_files = list(filter(lambda x: ".json" in x, json_files))
    print(json_files)

    # Create empty results file
    csv_file = "nvmf_results.csv"
    with open(os.path.join(output_dir, csv_file), "w") as fh:
        header_line = ",".join(["Name",
                                "read_iops", "read_bw", "read_avg_lat", "read_min_lat", "read_max_lat",
                                "write_iops", "write_bw", "write_avg_lat", "write_min_lat", "write_max_lat"])
        fh.write(header_line + "\n")

    for file in json_files:
        row_name, _ = os.path.splitext(file)

        # Regardless of number of threads used in fio config all results will
        # be aggregated into single entry due to use of "group_reporting" option,
        # so there will be only one entry with stats in json file
        job_pos = 0

        with open(os.path.join(output_dir, file), "r") as json_data:
            data = json.load(json_data)

            if "lat_ns" in data["jobs"][job_pos]["read"]:
                lat = "lat_ns"
                lat_units = "ns"
            else:
                lat = "lat"
                lat_units = "us"

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

        row_fields = [row_name,
                      read_iops, read_bw, read_avg_lat, read_min_lat, read_max_lat,
                      write_iops, write_bw, write_avg_lat, write_min_lat, write_max_lat]
        row_fields = [str(x) for x in row_fields]  # Make sure these are str for "join" operation
        row = ",".join(row_fields)

        with open(os.path.join(output_dir, csv_file), "a") as fh:
            fh.write(row + "\n")
