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
    cluster_size = "-c 1048576"  # 1MB cluster size for tests
    num_test = None
    fail_count = 0
    tc_failed = []
    tc_list = []

    if len(sys.argv) >= 5 and len(sys.argv) <= test_counter():
        rpc_py = sys.argv[1]
        total_size = int(sys.argv[2])
        block_size = int(sys.argv[3])
        base_dir_path = sys.argv[4]
        tc_list = sys.argv[5].split(',')
    else:
        print("Invalid argument")
    try:
        tc = TestCases(rpc_py, total_size, block_size, cluster_size, base_dir_path)

        if "all" in tc_list:
            for num_test in range(1, test_counter() + 1):
                fail_count = 0
                exec("fail_count += tc.test_case{num_test}"
                     "()".format(num_test=num_test))
                check_fail_count(fail_count, num_test)
                if fail_count:
                    tc_failed.append(num_test)
        else:
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
