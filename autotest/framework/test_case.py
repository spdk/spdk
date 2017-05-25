# BSD LICENSE
#
# Copyright(c) 2010-2014 Intel Corporation. All rights reserved.
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
#
#   * Redistributions of source code must retain the above copyright
#     notice, this list of conditions and the following disclaimer.
#   * Redistributions in binary form must reproduce the above copyright
#     notice, this list of conditions and the following disclaimer in
#     the documentation and/or other materials provided with the
#     distribution.
#   * Neither the name of Intel Corporation nor the names of its
#     contributors may be used to endorse or promote products derived
#     from this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

"""
A base class for creating DTF test cases.
"""
import os
import re
import sys
import traceback
from exception import VerifyFailure, TimeoutException
from settings import DRIVERS, NICS, get_nic_name, load_global_setting
from settings import PERF_SETTING, FUNC_SETTING, DEBUG_SETTING, DEBUG_CASE_SETTING, HOST_DRIVER_SETTING
from excel_reporter import ExcelReporter
from test_result import ResultTable, Result
from logger import getLogger
excel_report = None


class TestCase(object):

    def __init__(self, duts, tester, target, suitename):
        self.suite_name = suitename
        self.dut = duts[0]
        self.duts = duts
        self.tester = tester
        self.target = target
        class_name = self.__class__.__name__
        self.logger = getLogger(class_name)
        self.logger.config_suite(class_name)
        self._requested_tests = None
        self.nics = []
        drivername = []
        execution_path = os.path.dirname(os.path.dirname(__file__))
        execution_file = execution_path + '/framework/execution.cfg'
        execution = open(execution_file, 'r')
        status = re.findall(r"\n+parameters=nic_type=(.*)", execution.read())
        status_nic = status[0].split(":")
        self.nic = status_nic[0]
        self.kdriver = self._get_nic_driver(self.nic)
        self._suite_result = Result()
        self._suite_result.dut = self.dut.crb['IP']
        self._suite_result.target = target
        self._suite_result.nic = self.nic
        self._suite_result.test_suite = self.suite_name
        if self._suite_result is None:
            raise ValueError("Result object should not None")
        if load_global_setting(PERF_SETTING) == "yes":
            self._enable_perf = True
        else:
            self._enable_perf = False
        if load_global_setting(FUNC_SETTING) == "yes":
            self._enable_func = True
        else:
            self._enable_func = False
        if load_global_setting(DEBUG_SETTING) == "yes":
            self._enable_debug = True
        else:
            self._enable_debug = False
        if load_global_setting(DEBUG_CASE_SETTING) == "yes":
            self._debug_case = True
        else:
            self._debug_case = False
        self.drivername = load_global_setting(HOST_DRIVER_SETTING)

    def verify(self, passed, description):
        if not passed:
            raise VerifyFailure(description)

    def _get_nic_driver(self, nic_name):
        if nic_name in DRIVERS.keys():
            return DRIVERS[nic_name]
        return "Unknown"

    def result_table_create(self, header):
        self._result_table = ResultTable(header)
        self._result_table.set_logger(self.logger)

    def result_table_add(self, row):
        self._result_table.add_row(row)

    def result_table_print(self):
        self._result_table.table_print()

    def result_table_getrows(self):
        return self._result_table.results_table_rows

    def _get_functional_cases(self):
        """
        Get all functional test cases.
        """
        return self._get_test_cases(r'test_(?!perf_)')

    def _get_performance_cases(self):
        """
        Get all performance test cases.
        """
        return self._get_test_cases(r'test_perf_')

    def _has_it_been_requested(self, test_case, test_name_regex):
        """
        Check whether test case has been requested for validation.
        """
        name_matches = re.match(test_name_regex, test_case.__name__)
        if self._requested_tests is not None:
            return name_matches and test_case.__name__ in self._requested_tests
        return name_matches

    def set_requested_cases(self, case_list):
        """
        Pass down input cases list for check
        """
        self._requested_tests = case_list

    def _get_test_cases(self, test_name_regex):
        """
        Return case list which name matched regex.
        """
        for test_case_name in dir(self):
            test_case = getattr(self, test_case_name)
            if callable(test_case) and self._has_it_been_requested(
                    test_case, test_name_regex):
                yield test_case

    def execute_setup_all(self):
        """
        Execute suite setup_all function before cases.
        """
        for dutobj in self.duts:
            dutobj.get_session_output(timeout=0.1)
        self.tester.get_session_output(timeout=0.1)
        try:
            self.set_up_all()
            return True
        except Exception:
            self.logger.error('set_up_all failed:\n' + traceback.format_exc())
            if self._enable_func:
                for case_obj in self._get_functional_cases():
                    self._suite_result.test_case = case_obj.__name__
                    self._suite_result.test_case_blocked('set_up_all failed')
            if self._enable_perf:
                for case_obj in self._get_performance_cases():
                    self._suite_result.test_case = case_obj.__name__
                    self._suite_result.test_case_blocked('set_up_all failed')
            return False

    def _execute_test_case(self, case_obj):
        """
        Execute specified test case in specified suite. If any exception occured in
        validation process, save the result and tear down this case.
        """
        global excel_report
        case_name = case_obj.__name__
        self._suite_result.test_case = case_obj.__name__
        excel_report = ExcelReporter('../output/test_results.xls')
        is_backend = [
            "iscsi_rxtxqueue",
            "iscsi_multiconnection",
            "nvmf_rxtxqueue",
            "nvmf_multiconnection"]
        try:
            if self._suite_result.test_suite in is_backend:
                if re.findall(r'test_a_', case_name):
                    pass
                else:
                    return True
            self.logger.info('Test Case %s Begin' % case_name)
            self.running_case = case_name
            # clean session
            for dutobj in self.duts:
                dutobj.get_session_output(timeout=0.1)
            self.tester.get_session_output(timeout=0.1)
            # run set_up function for each case
            self.set_up()
            case_obj()
            self._suite_result.test_case_passed()
            excel_report.save(self._suite_result)
            self.logger.info('Test Case %s Result PASSED:' % case_name)
        except VerifyFailure as v:
            self._suite_result.test_case_failed(str(v))
            excel_report.save(self._suite_result)
            self.logger.error('Test Case %s Result FAILED: ' %
                              (case_name) + str(v))
        except KeyboardInterrupt:
            self._suite_result.test_case_blocked("Skipped")
            excel_report.save(self._suite_result)
            self.logger.error('Test Case %s SKIPED: ' % (case_name))
            self.tear_down()
            raise KeyboardInterrupt("Stop SPDK")
        except TimeoutException as e:
            msg = str(e)
            self._suite_result.test_case_failed(msg)
            excel_report.save(self._suite_result)
            self.logger.error('Test Case %s Result FAILED: ' %
                              (case_name) + msg)
            self.logger.error('%s' % (e.get_output()))
        except Exception:
            trace = traceback.format_exc()
            self._suite_result.test_case_failed(trace)
            excel_report.save(self._suite_result)
            self.logger.error('Test Case %s Result ERROR: ' %
                              (case_name) + trace)
        finally:
            self.tear_down()

    def execute_test_cases(self):
        """
        Execute all test cases in one suite.
        """
        if load_global_setting(FUNC_SETTING) == 'yes':
            for case_obj in self._get_functional_cases():
                self._execute_test_case(case_obj)
        if load_global_setting(PERF_SETTING) == 'yes':
            for case_obj in self._get_performance_cases():
                self._execute_test_case(case_obj)

    def get_result(self):
        return self._suite_result

    def execute_tear_downall(self):
        pass
