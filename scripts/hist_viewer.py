#!/usr/bin/python
# example usage: ./scripts/rpc.py hist_show ID | ./scripts/hist_viewer.py
import json
import re
import sys

json_data = None

def parse_json_data(raw_json):
    regex_replace = [(r"([ \{,:\[])(u)?'([^']+)'", r'\1"\3"'), (r" False([, \}\]])", r' false\1'), (r" True([, \}\]])", r' true\1')]
    for r, s in regex_replace:
        raw_json = re.sub(r, s, raw_json)
    corrected_json = json.loads(raw_json)
    return corrected_json

def show_histogram_header():
    global json_data
    id_hist = json_data["ID"]
    histogram_name = json_data["histogram_name"]
    class_name = json_data["class_name"]
    metric = json_data["Metric"]
    enabled = json_data["enabled"]
    timestamp_rate = long(json_data["timestamp_rate"])
    print "+++++++++++++++++   Header   ++++++++++++++++++++++++"
    print "%-24s: %d" % ("ID", id_hist)
    print "%-24s: %s" % ("Histogram name", histogram_name)
    print "%-24s: %s" % ("Class", class_name)
    print "%-24s: %s" % ("Unit", metric)
    print "%-24s: %s" % ("Enabled", enabled)
    print "+++++++++++++++++++++++++++++++++++++++++++++++++++++\n"

def show_histogram_bins():
    global json_data
    num_bucket_ranges = int(json_data["num_bucket_ranges"])
    num_bucket_per_range = json_data["num_bucket_per_range"]
    total_num_ios = long(json_data["total_num_ios"])
    histogram_raw_data = json_data["histogram_raw_data"]
    bin_header = "%-21s %-21s %-21s %-21s" % ("Min", "Max", "Frequency", "Percent%")
    print bin_header
    min_val = 0
    max_val = 1
    for i in range(num_bucket_ranges):
        for j in range(num_bucket_per_range):
            freq = histogram_raw_data[i * num_bucket_per_range + j]
            if total_num_ios:
                percent = (freq * 100) / float(total_num_ios)
            else:
                percent = 0
            if freq != 0:
                print "%-21lu %-21lu %-21lu %-21.3f" % (min_val, max_val, freq, percent)
            if (i < 2):
                min_val = min_val + 1
                max_val = min_val + 1
            else:
                min_val = min_val + 2 ** (i - 1)
                max_val = min_val + 2 ** (i - 1)

def show_histogram_statistics():
    global json_data
    max_value = json_data["max_value"]
    min_value = json_data["min_value"]
    total_values = long(json_data["total_values"])
    total_num_ios = long(json_data["total_num_ios"])
    print "+++++++++++++++++   Statistics   ++++++++++++++++++++++++"
    print "%-24s: %lu" % ("Total I/O", total_num_ios)
    print "%-24s: %lu" % ("Min Value", min_value)
    print "%-24s: %lu" % ("Max Value", max_value)
    if total_num_ios:
        print "%-24s: %.2f" % ("Avg Value", float(total_values / total_num_ios))
    else:
        print "%-24s: %.2f" % ("Avg Value", 0.0)

    print "+++++++++++++++++++++++++++++++++++++++++++++++++++++++++\n"

if __name__ == "__main__":
    input_json_data = sys.stdin.read()
    json_data = parse_json_data(input_json_data)
    show_histogram_header()
    show_histogram_statistics()
    show_histogram_bins()
