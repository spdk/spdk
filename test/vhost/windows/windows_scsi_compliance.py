#!/usr/bin/env python3
import os
import sys
import re
import pprint
import collections

os.chdir(os.path.join(os.path.dirname(sys.argv[0]), "results"))

scsi_logs = filter(lambda x: x.endswith(".log"), os.listdir("./"))
scsi_1_pattern = re.compile(r"(ASSERTION\s[1-9][\d+]?\.\d+\s)(.+\s)([\w\W]+?)(Result:\s)(\w+)", re.I | re.M)
scsi_2_pattern = re.compile(r"(?:Start:\s)(ASSERTION:\s)?(.+)(?:,.+=\s)([\w\W]+?)(End:\s)(\w+)(,.*)", re.I | re.M)
fails = []
warns = []

expected_warns = [
    "MODE_SELECT_6_MODE_SENSE_6_Checking_Parameters_Savable_PS_bit",
    "MODE_SELECT_10_MODE_SENSE_10_Checking_Parameters_Savable_PS_bit",
    "MODE_SELECT_10_Changing_WCE",
    "MODE_SELECT_10_MODE_SENSE_10_Checking_that_WCE_has_been_cleared",
    "MODE_SELECT_10_MODE_SENSE_10_Checking_that_Saved_Values_have_changed",
    "MODE_SELECT_10_setting_WCE",
    "MODE_SELECT_10_MODE_SENSE_10_Checking_that_WCE_has_been_set",
    "MODE_SELECT_10_Attempting_to_restore_original_values",
    "MODE_SELECT_10_MODE_SENSE_10_Verifying_values_were_restored",
    "ASSERTION_VERIFY_16_Support_Test",
]

expected_fails = [
    "ASSERTION_READ_6_Read-With-Disk-Cache-Cleared_Test",
    "ASSERTION_READ_10_Read-With-Disk-Cache-Cleared_Test",
    "ASSERTION_READ_16_Read-With-Disk-Cache-Cleared_Test",
    "ASSERTION_INQUIRY_Checking_Identification_Descriptors_in_VPD_page_0x83",
    "ASSERTION_VERIFY_10_Support_Test",
]

results = {"1": collections.OrderedDict(),
           "2": collections.OrderedDict()}

for log in scsi_logs:
    # Choose regex pattern depending on tests version
    pattern = scsi_1_pattern if "WIN_SCSI_1" in log else scsi_2_pattern

    # Read log file contents
    try:
        with open(log, 'r') as fh:
            fh = open(log, 'r')
            log_text = fh.read()
            # Dir name for saving split result files of currently processed log file
            d_name = log.split(".")[0]
            try:
                os.mkdir(d_name)
            except OSError:
                pass
    except IOError as e:
        print("ERROR: While opening log file: {log_file}".format(log_file=log))
        exit(1)

    # Parse log file contents
    matches_found = re.findall(pattern, log_text)
    if len(matches_found) < 1:
        print("ERROR: No results found in file {log_file}!".format(log_file=log))
        exit(1)

    # Go through output for each test from log file; parse and save to dict
    for m in matches_found:
        test_name = re.sub(r"\s+", "_", (m[0] + m[1]).strip())
        test_name = re.sub(r"[():]", "", test_name)
        test_name = test_name[0:-1] if "." in test_name[-1] else test_name
        tc_result = m[4].upper()

        if "FAIL" in tc_result.upper():
            fails.append([log, test_name, tc_result])
        elif "WARN" in tc_result.upper():
            warns.append([log, test_name, tc_result])

        # Save output to separate file
        with open(os.path.join("./", d_name, test_name), 'w') as fh:
            for line in m:
                fh.write(line)

        # Also save in dictionary for later use in generating HTML results summary
        ver = "1" if "WIN_SCSI_1" in log else "2"
        try:
            results[ver][test_name][d_name] = tc_result
        except KeyError:
            results[ver][test_name] = collections.OrderedDict()
            results[ver][test_name][d_name] = tc_result


# Generate HTML file with results table
with open(os.path.join("./", "results.html"), 'a') as fh:
    html = "<html>"
    for suite_ver in results.keys():
        html += """"<h2> WIN_SCSI_{ver} </h2>
        <table bgcolor=\"#ffffff\" border=\"1px solid black;>\"""".format(ver=suite_ver)

        # Print header
        html += "<tr><th>Test name</th>"
        disks_header = set()

        for _ in results[suite_ver].keys():
            for disk in results[suite_ver][_].keys():
                disks_header.add(disk)

        for disk in disks_header:
            html += "<th>{disk}</th>".format(disk=disk)
        html += "</tr>"

        # Print results
        for test in results[suite_ver].keys():
            html += "<tr><td>{f_name}</td>".format(f_name=test)
            for disk in disks_header:
                try:
                    result = results[suite_ver][test][disk]

                    html += "<td"
                    if "PASS" in result:
                        html += " bgcolor=\"#99ff33\">"
                    else:
                        html += " bgcolor=\"#ff5050\">"

                    html += "<a href={file}>{result}</a>".format(result=result, file=os.path.join("./", disk, test))
                    html += "</td>"

                except KeyError:
                    html += "<td bgcolor=\"#ffff99\"></br></td>"
            html += "</tr>"
        html += "</table></br>"
    html += "</html>"
    fh.write(html)

if warns:
    not_expected_warns = [w for w in warns if w[1] not in expected_warns and "WIN_SCSI_2" in w[0]]
    print("INFO: Windows SCSI compliance warnings:")
    pprint.pprint(warns, width=150)

if fails:
    not_expected_fails = [f for f in fails if f[1] not in expected_fails and "WIN_SCSI_2" in f[0]]
    print("INFO: Windows SCSI compliance fails:")
    pprint.pprint(fails, width=150)

if not_expected_warns or not_expected_fails:
    print("Not expected fails / warnings:")
    pprint.pprint(not_expected_warns, width=150)
    pprint.pprint(not_expected_fails, width=150)
    exit(1)
