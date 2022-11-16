#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2018 Intel Corporation.
#  All rights reserved.

import os
import re
import json
import logging
from subprocess import check_output
from collections import OrderedDict
from json.decoder import JSONDecodeError


def read_json_stats(file):
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


def read_target_stats(measurement_name, results_file_list, results_dir):
    # Read additional metrics measurements done on target side and
    # calculate the average from across all workload iterations.
    # Currently only works for SAR CPU utilization and power draw measurements.
    # Other (bwm-ng, pcm, dpdk memory) need to be refactored and provide more
    # structured result files instead of a output dump.
    total_util = 0
    for result_file in results_file_list:
        with open(os.path.join(results_dir, result_file), "r") as result_file_fh:
            total_util += float(result_file_fh.read())
    avg_util = total_util / len(results_file_list)

    return {measurement_name: "{0:.3f}".format(avg_util)}


def parse_results(results_dir, csv_file):
    files = os.listdir(results_dir)
    fio_files = filter(lambda x: ".fio" in x, files)
    json_files = [x for x in files if ".json" in x]
    sar_files = [x for x in files if "sar" in x and "util" in x]
    pm_files = [x for x in files if "pm" in x and "avg" in x]

    headers = ["read_iops", "read_bw", "read_avg_lat_us", "read_min_lat_us", "read_max_lat_us",
               "read_p99_lat_us", "read_p99.9_lat_us", "read_p99.99_lat_us", "read_p99.999_lat_us",
               "write_iops", "write_bw", "write_avg_lat_us", "write_min_lat_us", "write_max_lat_us",
               "write_p99_lat_us", "write_p99.9_lat_us", "write_p99.99_lat_us", "write_p99.999_lat_us"]

    header_line = ",".join(["Name", *headers])
    rows = set()

    for fio_config in fio_files:
        logging.info("Getting FIO stats for %s" % fio_config)
        job_name, _ = os.path.splitext(fio_config)
        aggr_headers = ["iops", "bw", "avg_lat_us", "min_lat_us", "max_lat_us",
                        "p99_lat_us", "p99.9_lat_us", "p99.99_lat_us", "p99.999_lat_us"]

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
        sar_result_files = [x for x in sar_files if x.startswith(job_name)]
        pm_result_files = [x for x in pm_files if x.startswith(job_name)]

        logging.info("Matching result files for current fio config %s:" % job_name)
        for j in job_result_files:
            logging.info("\t %s" % j)

        # There may have been more than 1 initiator used in test, need to check that
        # Result files are created so that string after last "_" separator is server name
        inits_names = set([os.path.splitext(x)[0].split("_")[-1] for x in job_result_files])
        inits_avg_results = []
        for i in inits_names:
            logging.info("\tGetting stats for initiator %s" % i)
            # There may have been more than 1 test run for this job, calculate average results for initiator
            i_results = [x for x in job_result_files if i in x]
            i_results_filename = re.sub(r"run_\d+_", "", i_results[0].replace("json", "csv"))

            separate_stats = []
            for r in i_results:
                try:
                    stats = read_json_stats(os.path.join(results_dir, r))
                    separate_stats.append(stats)
                    logging.info(stats)
                except JSONDecodeError:
                    logging.error("ERROR: Failed to parse %s results! Results might be incomplete!" % r)

            init_results = [sum(x) for x in zip(*separate_stats)]
            init_results = [x / len(separate_stats) for x in init_results]
            inits_avg_results.append(init_results)

            logging.info("\tAverage results for initiator %s" % i)
            logging.info(init_results)
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

        if sar_result_files:
            aggr_headers.append("target_avg_cpu_util")
            aggregate_results.update(read_target_stats("target_avg_cpu_util", sar_result_files, results_dir))

        if pm_result_files:
            aggr_headers.append("target_avg_power")
            aggregate_results.update(read_target_stats("target_avg_power", pm_result_files, results_dir))

        rows.add(",".join([job_name, *aggregate_results.values()]))

    # Create empty results file with just the header line
    aggr_header_line = ",".join(["Name", *aggr_headers])
    with open(os.path.join(results_dir, csv_file), "w") as fh:
        fh.write(aggr_header_line + "\n")

    # Save results to file
    for row in rows:
        with open(os.path.join(results_dir, csv_file), "a") as fh:
            fh.write(row + "\n")
    logging.info("You can find the test results in the file %s" % os.path.join(results_dir, csv_file))
