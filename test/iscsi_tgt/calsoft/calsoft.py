#!/usr/bin/env python3
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2016 Intel Corporation
#  All rights reserved.
#

import os
import time
import sys
import subprocess
import threading
import json

CALSOFT_BIN_PATH = "/usr/local/calsoft/iscsi-pcts-v1.5/bin"

'''
11/26/2015 disable tc_login_11_2 and tc_login_11_4
RFC 7143 6.3
Neither the initiator nor the target should attempt to declare or
negotiate a parameter more than once during login, except for
responses to specific keys that explicitly allow repeated key
declarations (e.g., TargetAddress)

The spec didn't make it clear what other keys could be re-declare
Discussed this with UNH and get the conclusion that TargetName/
TargetAddress/MaxRecvDataSegmentLength could be re-declare.
'''
'''
12/1/2015 add tc_login_2_2 to known_failed_cases
RFC 7143 6.1
A standard-label MUST begin with a capital letter and must not exceed
63 characters.
key name: A standard-label
'''
'''
06/10/2020 add tc_login_29_1 to known_failed_cases
RFC 3720 12.19. DataSequenceInOrder
Irrelevant when: SessionType=Discovery
'''

known_failed_cases = ['tc_ffp_15_2', 'tc_ffp_29_2', 'tc_ffp_29_3', 'tc_ffp_29_4',
                      'tc_err_1_1', 'tc_err_1_2', 'tc_err_2_8',
                      'tc_err_3_1', 'tc_err_3_2', 'tc_err_3_3',
                      'tc_err_3_4', 'tc_err_5_1', 'tc_login_3_1',
                      'tc_login_11_2', 'tc_login_11_4', 'tc_login_2_2', 'tc_login_29_1']


def run_case(case, result_list, log_dir_path):
    try:
        case_log = subprocess.check_output("{}/{}".format(CALSOFT_BIN_PATH, case), stderr=subprocess.STDOUT, shell=True).decode('utf-8')
    except subprocess.CalledProcessError as e:
        result_list.append({"Name": case, "Result": "FAIL"})
        case_log = e.output.decode('utf-8')
    else:
        result_list.append({"Name": case, "Result": "PASS"})
    with open(log_dir_path + case + '.txt', 'w') as f:
        f.write(case_log)


def main():
    if not os.path.exists(CALSOFT_BIN_PATH):
        print("The Calsoft test suite is not available on this machine.")
        sys.exit(1)

    output_dir = sys.argv[1]
    if len(sys.argv) > 2:
        output_file = sys.argv[2]
    else:
        output_file = "%s/calsoft.json" % (output_dir)

    log_dir = "%s/calsoft/" % output_dir

    all_cases = [x for x in os.listdir(CALSOFT_BIN_PATH) if x.startswith('tc')]
    all_cases.sort()
    nopin_cases = ['tc_err_2_4', 'tc_err_8_2', 'tc_ffp_7_6_1', 'tc_ffp_7_7_3', 'tc_ffp_7_7_2',
                   'tc_ffp_7_7_5', 'tc_ffp_7_6_4', 'tc_ffp_7_6_3', 'tc_ffp_7_7_4', 'tc_ffp_7_6_2',
                   'tc_ffp_7_7_1', 'tc_ffp_7_7']

    case_result_list = []

    result = {"Calsoft iSCSI tests": case_result_list}

    if not os.path.exists(log_dir):
        os.mkdir(log_dir)
    for case in known_failed_cases:
        print("Skipping %s. It is known to fail." % (case))
        case_result_list.append({"Name": case, "Result": "SKIP"})

    thread_objs = []

    # The Calsoft tests all pull their InitiatorName from the its.conf file.  We set the
    # AllowDuplicatedIsid flag in the SPDK JSON config, to allow these tests to run in
    # parallel where needed, but we only run tests in parallel that make sense - in this case
    # only the nopin-related tests which take longer to run because of various timeouts.
    serial_cases = list(set(all_cases) - set(known_failed_cases) - set(nopin_cases))
    parallel_cases = nopin_cases

    for test_case in serial_cases:
        thread_obj = threading.Thread(target=run_case, args=(test_case, case_result_list, log_dir, ))
        thread_obj.start()
        thread_obj.join(30)
        if thread_obj.is_alive():
            # Thread is still alive, meaning the join() timeout expired.
            print("Thread timeout")
            exit(1)

    for test_case in parallel_cases:
        thread_obj = threading.Thread(target=run_case, args=(test_case, case_result_list, log_dir, ))
        thread_obj.start()
        time.sleep(0.02)
        thread_objs.append(thread_obj)

    end_time = time.time() + 30
    while time.time() < end_time:
        for thread_obj in thread_objs:
            if thread_obj.is_alive():
                break
        else:
            break
    else:
        print("Thread timeout")
        exit(1)
    with open(output_file, 'w') as f:
        json.dump(obj=result, fp=f, indent=2)

    failed = 0
    for x in case_result_list:
        if x["Result"] == "FAIL":
            print("Test case %s failed." % (x["Name"]))
            failed = 1
    exit(failed)


if __name__ == '__main__':
    main()
