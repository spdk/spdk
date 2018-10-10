import os
import re
import json
from itertools import product, chain
from subprocess import check_output, Popen


def run_process(command):
    return check_output(command, shell=True).decode("utf-8")


def get_used_numa_nodes():
    used_numa_nodes = set()
    for bdf in get_nvme_devices_bdf():
        output = check_output("lspci -vv -s %s | grep -i NUMA" % bdf, shell=True).decode(encoding="utf-8")
        output = "".join(filter(lambda x: x.isdigit(), output))  # Join because filter returns an iterable
        used_numa_nodes.add(int(output))
    return used_numa_nodes


def get_nvme_devices_count():
    output = check_output("lspci | grep -i Non | wc -l", shell=True)
    return int(output)


def get_nvme_devices_bdf():
    print("Getting BDFs for NVMe section")
    output = check_output("lspci | grep -i Non | awk '{print $1}'", shell=True)
    output = [str(x, encoding="utf-8") for x in output.split()]
    print("Done getting BDFs")
    return output


def get_nvme_devices():
    print("Getting kernel NVMe names")
    output = check_output("lsblk -o NAME -nlp", shell=True).decode(encoding="utf-8")
    output = [x for x in output.split("\n") if "nvme" in x]
    print("Done getting kernel NVMe names")
    return output


def gen_core_mask(num_cpus, numa_list):
    cpu_per_socket = check_output('lscpu | grep -oP "per socket:\s+\K\d+"', shell=True).decode("utf-8")
    cpu_per_socket = int(cpu_per_socket)
    cpu_socket_ranges = [list(range(x * cpu_per_socket, cpu_per_socket + x * cpu_per_socket)) for x in numa_list]

    # Mix CPUs in Round robin fashion
    mixed_cpus = list(chain.from_iterable(zip(*cpu_socket_ranges)))
    cpu_list = mixed_cpus[0:num_cpus]
    cpu_list = [str(x) for x in cpu_list]
    cpu_list = "[" + ",".join(cpu_list) + "]"
    return cpu_list


def nvmet_command(command, dir_path=None):
    if dir_path:
        nvmetcli_path = os.path.join(dir_path, "nvmetcli")
    else:
        nvmetcli_path = "nvmetcli"
    return check_output("%s %s" % (nvmetcli_path, command), shell=True).decode(encoding="utf-8")


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


def discover_subsystems(address_list):
    nvme_discover_cmd = ["sudo", "nvme", "discover", "-t rdma", "-s 4420", "-a %s" % address_list[0]]
    nvme_discover_cmd = " ".join(nvme_discover_cmd)
    nvme_discover_output = check_output(nvme_discover_cmd, shell=True)

    # Call nvme discover and get trsvcid, traddr and subnqn fields. Filter out any not matching IP entries
    subsystems = re.findall(r'trsvcid:\s(\d+)\s+'  # get svcid number
                            r'subnqn:\s+([a-zA-Z0-9\.\-\:]+)\s+'  # get NQN id
                            r'traddr:\s+(\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3})',  # get IP address
                            str(nvme_discover_output, encoding="utf-8"))  # from nvme discovery output

    # Filter out entries which ip's don't mach expected
    # x[-1] because last field is ip address
    subsystems = filter(lambda x: x[-1] in address_list, subsystems)
    subsystems = list(subsystems)

    return subsystems


# TODO: Kernel target behaves differently than SPDK target
# It does not allow multiple subsystems to be located on the same
# IP / Port pair so it's not possible to discover multiple subsystems at once.
# Need to check for open subsystems.
def discover_kernel_subsystems(address_list):
    num_nvmes = range(0, 9)
    nvme_discover_output = ""
    for ip, subsys_no in product(address_list, num_nvmes):
        nvme_discover_cmd = ["sudo", "nvme", "discover", "-t rdma", "-s %s" % (4421 + subsys_no), "-a %s" % ip]
        nvme_discover_cmd = " ".join(nvme_discover_cmd)
        try:
            out = check_output(nvme_discover_cmd, shell=True).decode(encoding="utf-8")
            nvme_discover_output = "\n".join([nvme_discover_output, out])
        except:
            pass

    subsystems = re.findall(r'trsvcid:\s(\d+)\s+'  # get svcid number
                            r'subnqn:\s+([a-zA-Z0-9\.\-\:]+)\s+'  # get NQN id
                            r'traddr:\s+(\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3})',  # get IP address
                            nvme_discover_output)  # from nvme discovery output
    subsystems = filter(lambda x: x[-1] in address_list, subsystems)
    subsystems = list(subsystems)
    return subsystems


def run_fio(block_size, iodepth, rw, rw_mix, cpus_num, run_num, output_dir=""):
    print("Running FIO test with args:")

    if output_dir:
        try:
            os.mkdir(output_dir)
        except FileExistsError:
            pass

    # TODO: Fix _c_CPUS_NUM field. Currently need to set it to "all" if no num cores specified.
    # It should be omitted if no restriction on cpus
    for i in range(1, run_num + 1):
        output_file = "s_" + str(block_size) + "_q_" + str(iodepth) + "_m_" + str(rw) + "_c_" + str(cpus_num) + "_run_" + str(i) + ".json"
        output_file = os.path.join(output_dir, output_file)  # Add output dir if it's available
        command = "sudo /usr/src/fio/fio %s --output-format=json --output=%s" % ("fio.conf", output_file)
        print(command)

        proc = Popen(command, shell=True)
        proc.wait()

    print("Finished Test: IO Size={} QD={} Mix={} CPU Mask={}".format(block_size, iodepth, rw_mix, cpus_num))
    return