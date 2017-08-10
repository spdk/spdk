#!/usr/bin/env python
import json
import re
import sys
import os
import argparse

json_data = None
graph_enabled = False
# if ticks are used to track time, then use it otherwise default 1, so
# that other metric histograms are unaffected by this multiplying factor.
if_ticks_to_nsec = 1

def plot_histogram(x, y, z, diaplay_file):
    import matplotlib.pyplot as plt
    fig = plt.figure(figsize=(16, 9))
    ax1 = fig.add_subplot(111)
    ax1.spines["top"].set_visible(False)
    ax1.spines["right"].set_visible(False)
    ax1.get_xaxis().tick_bottom()
    ax1.get_yaxis().tick_left()
    ax1.set_xlabel("Bins", fontsize=16)
    ax1.set_yticks(range(0, max(y), max(y) / 10))
    ax1.bar(x, y, width=0.8, color="#3F5D7D", align="center", linewidth=0)
    ax1.set_ylabel('Frequency', fontsize=16)
    ax2 = ax1.twinx()
    ax2.get_yaxis().tick_right()
    ax2.set_yticks(range(0, 101, 5))
    ax2.plot(x, z, color="g", linewidth=3.0)
    ax2.set_ylabel('Percentile', fontsize=16)
    plt.xticks(fontsize=14)
    plt.yticks(fontsize=14)
    plt.title('Histogram (freq > 0.001%)', fontsize=20)
    plt.savefig(diaplay_file)


def show_histogram_header(out_fp):
    global json_data
    global if_ticks_to_nsec
    id_hist = int(json_data["ID"])
    histogram_name = json_data["histogram_name"]
    class_name = json_data["class_name"]
    metric = json_data["metric"]
    if metric == "ticks":
        metric = "nsec"
        timestamp_rate = long(json_data["timestamp_rate"])
        if_ticks_to_nsec = (1000 * 1000 * 1000) / float(timestamp_rate) if timestamp_rate != 0 else 0
    enabled = json_data["enabled"]
    print >>out_fp, "+++++++++++++++++   Header   ++++++++++++++++++++++++"
    print >>out_fp, "%-24s: %d" % ("ID", id_hist)
    print >>out_fp, "%-24s: %s" % ("Histogram name", histogram_name)
    print >>out_fp, "%-24s: %s" % ("Class", class_name)
    print >>out_fp, "%-24s: %s" % ("Unit", metric)
    print >>out_fp, "%-24s: %s" % ("Enabled", enabled)
    print >>out_fp, "+++++++++++++++++++++++++++++++++++++++++++++++++++++\n"


def show_histogram_bins(out_fp):
    global json_data
    global if_ticks_to_nsec
    x = []
    y = []
    z = []
    percentile = 0.0
    total_num_ios = long(json_data["total_num_ios"])
    histogram_raw_data = json_data["histogram_data"]
    bin_header = "%-21s %-21s %-21s %-21s %-21s" % ("Min", "Max", "Frequency", "Percent%", "Percentile")
    print >>out_fp, bin_header
    for hist_bin in histogram_raw_data:
        min_val = long(hist_bin["min"])
        max_val = long(hist_bin["max"])
        freq = long(hist_bin["count"])
        if (json_data["metric"] == "ticks"):
            min_val = float(min_val * if_ticks_to_nsec)
            max_val = float(max_val * if_ticks_to_nsec)
        percent = (freq * 100) / float(total_num_ios) if total_num_ios != 0 else 0
        percentile = percentile + percent
        print >>out_fp, "%-21lu %-21lu %-21lu %-21.3f %-21.3f" % (min_val, max_val, freq, percent, percentile)
        if graph_enabled:
            if (percent > 0.05):
                x.append(min_val)
                y.append(freq)
                z.append(percentile)
    return x, y, z

def show_histogram_statistics(out_fp):
    global json_data
    global if_ticks_to_nsec
    max_value = long(json_data["max_value"])
    min_value = long(json_data["min_value"])
    total_values = long(json_data["total_values"])
    total_num_ios = long(json_data["total_num_ios"])
    print >>out_fp, "+++++++++++++++++   Statistics   ++++++++++++++++++++++++"
    print >>out_fp, "%-24s: %lu" % ("Total I/O", total_num_ios)
    print >>out_fp, "%-24s: %lu" % ("Min Value", min_value * if_ticks_to_nsec)
    print >>out_fp, "%-24s: %lu" % ("Max Value", max_value * if_ticks_to_nsec)
    print >>out_fp, "%-24s: %.2f" % ("Avg Value", total_values * if_ticks_to_nsec / float(total_num_ios) if total_num_ios != 0 else 0)
    print >>out_fp, "+++++++++++++++++++++++++++++++++++++++++++++++++++++++++\n"

def load_json_data(raw_json):
    return json.loads(raw_json)

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description='utility to view histogram generated from scripts/rpc.py')
    parser.add_argument('-i', '--input_file', help='Histogram input file or stdin')
    parser.add_argument('-o', '--output_file', help='Histogram output file or stdout')
    parser.add_argument('-g', '--graph', action='store_true', help='Enable histogram graph')
    parser.add_argument('-d', "--display_graph_file", help="File to store histogram graph")
    args = vars(parser.parse_args())
    if args["input_file"] is not None:
        in_fp = open(args["input_file"], 'r')
    else:
        in_fp = sys.stdin

    if args["output_file"] is not None:
        out_fp = open(args["output_file"], 'w')
    else:
        out_fp = sys.stdout

    if args["graph"]:
        if args["display_graph_file"] is None:
            parser.error("-g/--graph requires -d/--display_graph_file=<filename.png>")
        else:
            if os.path.splitext(args["display_graph_file"])[1] != ".png":
                parser.error("-d/--display_graph_file=<filename.png> requires .png file")
            else:
                graph_enabled = True
    input_json_data = in_fp.read()
    json_data = load_json_data(input_json_data)
    show_histogram_header(out_fp)
    show_histogram_statistics(out_fp)
    x, y, z = show_histogram_bins(out_fp)
    if graph_enabled:
        plot_histogram(x, y, z, args["display_graph_file"])
