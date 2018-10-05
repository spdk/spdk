#!/usr/bin/env python3

# This script runs fio benchmark test on the local nvme device using the SPDK NVMe driver.
# Prework: Run script/setup.sh to bind SSDs to SPDK driver.
# Prework: Change any fio configurations in the template fio config file fio_test.conf
# Output: A csv file <hostname>_<num ssds>_perf_output.csv

import subprocess
from subprocess import check_call, call, check_output, Popen, PIPE
import random
import os
import sys
import re
import signal
import getopt
from datetime import datetime
from itertools import *
import csv
import itertools
from shutil import copyfile
import json

# Populate test parameters into these lists to run different workloads
# The configuration below runs QD 1 & 128. To add QD 32 set q_depth=['1', '32', '128']
q_depth = ['1', '128']
# io_size specifies the size in bytes of the IO workload.
# To add 64K IOs set io_size = ['4096', '65536']
io_size = ['4096']
workload_type = ['randrw']
mix = ['100']
core_mask = ['0x1']
# run_time parameter specifies how long to run each test.
# Set run_time = ['600'] to run the test for 10 minutes
run_time = ['60']
# iter_num parameter is used to run the test multiple times.
# set iter_num = ['1', '2', '3'] to repeat each test 3 times
iter_num = ['1']


def run_fio(io_size_bytes, qd, rw_mix, cpu_mask, run_num, workload, run_time_sec):
    print("Running Test: IO Size={} QD={} Mix={} CPU Mask={}".format(io_size_bytes, qd, rw_mix, cpu_mask))
    string = "s_" + str(io_size_bytes) + "_q_" + str(qd) + "_m_" + str(rw_mix) + "_c_" + str(cpu_mask) + "_run_" + str(run_num)

    # Call fio
    path_to_fio_conf = config_file_for_test
    path_to_ioengine = sys.argv[2]
    command = "BLK_SIZE=" + str(io_size_bytes) + " RW=" + str(workload) + " MIX=" + str(rw_mix) \
        + " IODEPTH=" + str(qd) + " RUNTIME=" + str(run_time_sec) + " IOENGINE=" + path_to_ioengine \
        + " fio " + str(path_to_fio_conf) + " -output=" + string + " -output-format=json"
    output = subprocess.check_output(command, shell=True)

    print("Finished Test: IO Size={} QD={} Mix={} CPU Mask={}".format(io_size_bytes, qd, rw_mix, cpu_mask))
    return


def parse_results(io_size_bytes, qd, rw_mix, cpu_mask, run_num, workload, run_time_sec):
    results_array = []

    # If json file has results for multiple fio jobs pick the results from the right job
    job_pos = 0

    # generate the next result line that will be added to the output csv file
    results = str(io_size_bytes) + "," + str(qd) + "," + str(rw_mix) + "," \
        + str(workload) + "," + str(cpu_mask) + "," + str(run_time_sec) + "," + str(run_num)

    # Read the results of this run from the test result file
    string = "s_" + str(io_size_bytes) + "_q_" + str(qd) + "_m_" + str(rw_mix) + "_c_" + str(cpu_mask) + "_run_" + str(run_num)
    with open(string) as json_file:
        data = json.load(json_file)
        job_name = data['jobs'][job_pos]['jobname']
        # print "FIO job name: ", job_name
        if 'lat_ns' in data['jobs'][job_pos]['read']:
            lat = 'lat_ns'
            lat_units = 'ns'
        else:
            lat = 'lat'
            lat_units = 'us'
        read_iops = float(data['jobs'][job_pos]['read']['iops'])
        read_bw = float(data['jobs'][job_pos]['read']['bw'])
        read_avg_lat = float(data['jobs'][job_pos]['read'][lat]['mean'])
        read_min_lat = float(data['jobs'][job_pos]['read'][lat]['min'])
        read_max_lat = float(data['jobs'][job_pos]['read'][lat]['max'])
        write_iops = float(data['jobs'][job_pos]['write']['iops'])
        write_bw = float(data['jobs'][job_pos]['write']['bw'])
        write_avg_lat = float(data['jobs'][job_pos]['write'][lat]['mean'])
        write_min_lat = float(data['jobs'][job_pos]['write'][lat]['min'])
        write_max_lat = float(data['jobs'][job_pos]['write'][lat]['max'])
        print("%-10s" % "IO Size", "%-10s" % "QD", "%-10s" % "Mix",
              "%-10s" % "Workload Type", "%-10s" % "CPU Mask",
              "%-10s" % "Run Time", "%-10s" % "Run Num",
              "%-15s" % "Read IOps",
              "%-10s" % "Read MBps", "%-15s" % "Read Avg. Lat(" + lat_units + ")",
              "%-15s" % "Read Min. Lat(" + lat_units + ")", "%-15s" % "Read Max. Lat(" + lat_units + ")",
              "%-15s" % "Write IOps",
              "%-10s" % "Write MBps", "%-15s" % "Write Avg. Lat(" + lat_units + ")",
              "%-15s" % "Write Min. Lat(" + lat_units + ")", "%-15s" % "Write Max. Lat(" + lat_units + ")")
        print("%-10s" % io_size_bytes, "%-10s" % qd, "%-10s" % rw_mix,
              "%-10s" % workload, "%-10s" % cpu_mask, "%-10s" % run_time_sec,
              "%-10s" % run_num, "%-15s" % read_iops, "%-10s" % read_bw,
              "%-15s" % read_avg_lat, "%-15s" % read_min_lat, "%-15s" % read_max_lat,
              "%-15s" % write_iops, "%-10s" % write_bw, "%-15s" % write_avg_lat,
              "%-15s" % write_min_lat, "%-15s" % write_max_lat)
        results = results + "," + str(read_iops) + "," + str(read_bw) + "," \
            + str(read_avg_lat) + "," + str(read_min_lat) + "," + str(read_max_lat) \
            + "," + str(write_iops) + "," + str(write_bw) + "," + str(write_avg_lat) \
            + "," + str(write_min_lat) + "," + str(write_max_lat)
        with open(result_file_name, "a") as result_file:
            result_file.write(results + "\n")
        results_array = []
    return


def get_nvme_devices_count():
    output = check_output('lspci | grep -i Non | wc -l', shell=True)
    return int(output)


def get_nvme_devices_bdf():
    output = check_output('lspci | grep -i Non | awk \'{print $1}\'', shell=True).decode("utf-8")
    output = output.split()
    return output


def add_filename_to_conf(conf_file_name, bdf):
    filestring = "filename=trtype=PCIe traddr=0000." + bdf.replace(":", ".") + " ns=1"
    with open(conf_file_name, "a") as conf_file:
        conf_file.write(filestring + "\n")


if len(sys.argv) != 4:
    print("usage: " % sys.argv[0] % " path_to_fio_conf path_to_ioengine num_ssds")
    sys.exit()

num_ssds = int(sys.argv[3])
if num_ssds > get_nvme_devices_count():
    print("System does not have {} NVMe SSDs.".format(num_ssds))
    sys.exit()

host_name = os.uname()[1]
result_file_name = host_name + "_" + sys.argv[3] + "ssds_perf_output.csv"

bdf = get_nvme_devices_bdf()
config_file_for_test = sys.argv[1] + "_" + sys.argv[3] + "ssds"
copyfile(sys.argv[1], config_file_for_test)

# Add the number of threads to the fio config file
with open(config_file_for_test, "a") as conf_file:
    conf_file.write("numjobs=" + str(1) + "\n")

# Add the NVMe bdf to the fio config file
for i in range(0, num_ssds):
    add_filename_to_conf(config_file_for_test, bdf[i])

# Set up for output
columns = "IO_Size,Q_Depth,Workload_Mix,Workload_Type,Core_Mask,Run_Time,Run,Read_IOPS,Read_bw(KiB/s), \
    Read_Avg_lat(us),Read_Min_Lat(us),Read_Max_Lat(us),Write_IOPS,Write_bw(KiB/s),Write_Avg_lat(us), \
    Write_Min_Lat(us),Write_Max_Lat(us)"

with open(result_file_name, "w+") as result_file:
    result_file.write(columns + "\n")

for i, (s, q, m, w, c, t) in enumerate(itertools.product(io_size, q_depth, mix, workload_type, core_mask, run_time)):
    run_fio(s, q, m, c, i, w, t)
    parse_results(s, q, m, c, i, w, t)

result_file.close()
