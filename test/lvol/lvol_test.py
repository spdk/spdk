#!/usr/bin/env python
import sys
from test_cases import *


def check_fail_count(fail_count, num_test):
    if not fail_count:
        print("Test: {num_test} - PASS".format(num_test=num_test))
    else:
        print("Test: {num_test} - FAIL".format(num_test=num_test))


if __name__ == "__main__":
    rpc_py = None
    total_size = None
    block_size = None
    cluster_size = None
    num_test = None
    fail_count = 0
    tc_failed = []
    tc_list = []

    if len(sys.argv) == 8 and len(sys.argv[7].split(',')) <= test_counter():
        rpc_py = sys.argv[1]
        total_size = int(sys.argv[2])
        block_size = int(sys.argv[3])
        cluster_size = int(sys.argv[4])
        base_dir_path = sys.argv[5]
        app_path = sys.argv[6]
        tc_list = sys.argv[7].split(',')
    else:
        print("Invalid argument")
    try:
        tc = TestCases(rpc_py, total_size, block_size, cluster_size, base_dir_path, app_path)

        if "all" in tc_list:
            tc_list = sorted([i.split("test_case")[1] for i in dir(TestCases) if "test_case" in i], key=int)

        for num_test in tc_list:
            fail_count = 0
            exec("fail_count += tc.test_case{num_test}"
                 "()".format(num_test=num_test))
            check_fail_count(fail_count, num_test)
            if fail_count:
                tc_failed.append(num_test)

        if not tc_failed:
            print("RESULT: All test cases - PASS")
        elif tc_failed:
            print("RESULT: Some test cases FAIL")
            print(tc_failed)
            sys.exit(1)
    except:
        print("Test: {num_test} - FAIL".format(num_test=num_test))
        sys.exit(1)
