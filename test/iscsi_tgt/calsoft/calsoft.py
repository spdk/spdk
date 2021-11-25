#!/usr/bin/env python3

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

    case_result_list = []

    result = {"Calsoft iSCSI tests": case_result_list}

    if not os.path.exists(log_dir):
        os.mkdir(log_dir)
    for case in known_failed_cases:
        print("Skipping %s. It is known to fail." % (case))
        case_result_list.append({"Name": case, "Result": "SKIP"})

    thread_objs = []
    left_cases = list(set(all_cases) - set(known_failed_cases))
    index = 0
    max_thread_count = 32

    while index < len(left_cases):
        cur_thread_count = 0
        for thread_obj in thread_objs:
            if thread_obj.is_alive():
                cur_thread_count += 1
        while cur_thread_count < max_thread_count and index < len(left_cases):
            thread_obj = threading.Thread(target=run_case, args=(left_cases[index], case_result_list, log_dir, ))
            thread_obj.start()
            time.sleep(0.02)
            thread_objs.append(thread_obj)
            index += 1
            cur_thread_count += 1
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
