#!/usr/bin/env python
import sys
from test_cases import *

if __name__ == "__main__":
    rpc_py = None
    total_size = None
    block_size = None
    fail_count = 0
    tc_list = []
    if len(sys.argv) >= 5 and len(sys.argv) <= 21:
        rpc_py = sys.argv[1]
        total_size = int(sys.argv[2])
        block_size = int(sys.argv[3])
        tc_list = sys.argv[4].split(',')
    else:
        print("Invalid argument")
    try:
        tc = TestCases(rpc_py, total_size, block_size)
        if "all" in tc_list:
            for count in range(1, 22):
                exec("fail_count += tc.test_case{num_test}"
                     "()".format(num_test=count))
        else:
            for num_test in tc_list:
                exec ("fail_count += tc.test_case{num_test}"
                      "()".format(num_test=num_test))
        if not fail_count:
            print("RESULT: All test case - PASSED")
    except:
        print("Failed tests")
        raise
