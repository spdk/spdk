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
Excel spreadsheet generator
"""
import xlwt
import xlrd
from test_result import Result
from xlwt.ExcelFormula import Formula


class ExcelReporter(object):

    def __init__(self, filename):
        self.filename = filename
        self.xsl_file = None
        self.result = None
        self.__styles()
        self.titles = [	{'row': 0, 'col': 0, 'width': 4000, 'title': 'DUT', 'style': self.header_style},
                        {'row': 0, 'col': 1, 'width': 7500,
                            'title': 'Target', 'style': self.header_style},
                        {'row': 0, 'col': 2, 'width': 3000,
                            'title': 'NIC', 'style': self.header_style},
                        {'row': 0,
                         'col': 3,
                         'width': 5000,
                         'title': 'Test suite',
                         'style': self.header_style},
                        {'row': 0, 'col': 4, 'width': 8000,
                            'title': 'Test case', 'style': self.header_style},
                        {'row': 0, 'col': 5, 'width': 3000,
                            'title': 'Results', 'style': self.header_style},
                        {'row': 0, 'col': 7, 'width': 1000,
                            'title': 'Pass', 'style': self.header_style},
                        {'row': 0, 'col': 8, 'width': 3000,
                            'title': 'Fail', 'style': self.header_style},
                        {'row': 0, 'col': 9, 'width': 3000,
                            'title': 'Blocked', 'style': self.header_style},
                        {'row': 0, 'col': 10, 'width': 3000,
                            'title': 'Not Run', 'style': self.header_style},
                        {'row': 0, 'col': 11, 'width': 3000,
                            'title': 'Total', 'style': self.header_style},
                        ]

    def __get_col_by_title(self, title):
        cols = []
        for ti in self.titles:
            if ti['title'] == title:
                cols.append(ti['col'])
        return cols

    def __write_init(self):
        self.workbook = xlwt.Workbook()
        self.sheet = self.workbook.add_sheet(
            "Test Results", cell_overwrite_ok=True)

    def __read_init(self):
        self.row = 0
        self.col = 0
        try:
            self.workboot = xlrd.open_workbook(self.filename)
            self.rsheet = self.workboot.sheet_by_name("Test Results")
        except Exception as e:
            print "FAILED TO LOAD EXCEL FILE %s: %s" % (self.filename, e)

    def __add_header(self):
        for title in self.titles:
            self.sheet.write(
                title['row'],
                title['col'],
                title['title'],
                title['style'])
        self.sheet.write(1, self.__get_col_by_title('Pass')[
                         0], Formula('COUNTIF(F2:F2000,"PASSED")'))
        self.sheet.write(1, self.__get_col_by_title('Fail')[0], Formula(
            'COUNTIF(F2:F2000,"FAILED*") + COUNTIF(F2:F2000,"IXA*")'))
        self.sheet.write(1, self.__get_col_by_title(
            'Blocked')[0], Formula('H2+I2+J2+K2'))
        for title in self.titles:
            self.sheet.col(title['col']).width = title['width']

    def __styles(self):
        header_pattern = xlwt.Pattern()
        header_pattern.pattern = xlwt.Pattern.SOLID_PATTERN
        header_pattern.pattern_fore_colour = xlwt.Style.colour_map[
            'ocean_blue']
        passed_font = xlwt.Font()
        passed_font.colour_index = xlwt.Style.colour_map['black']
        self.passed_style = xlwt.XFStyle()
        self.passed_style.font = passed_font
        failed_font = xlwt.Font()
        failed_font.bold = True
        failed_font.colour_index = xlwt.Style.colour_map['red']
        self.failed_style = xlwt.XFStyle()
        self.failed_style.font = failed_font
        header_font = xlwt.Font()
        header_font.bold = True
        header_font.height = 260
        header_font.italic = True
        header_font.colour_index = xlwt.Style.colour_map['white']
        title_font = xlwt.Font()
        title_font.bold = True
        title_font.height = 220
        title_font.italic = True
        self.header_style = xlwt.XFStyle()
        self.header_style.font = header_font
        self.header_style.pattern = header_pattern
        self.title_style = xlwt.XFStyle()
        self.title_style.font = title_font

    def __write_result(self, dut, target, suite, case):
        test_result = self.result.result_for(dut, target, suite, case)
        if test_result is not None and len(test_result) > 0:
            result = test_result[0]
            if test_result[1] != '':
                result = "{0} '{1}'".format(result, test_result[1])
            if test_result[0] == 'PASSED':
                self.sheet.write(self.row, self.col + 1, result)
            else:
                self.sheet.write(
                    self.row, self.col + 1, result, self.failed_style)

    def __write_cases(self, dut, target, suite):
        for case in self.result.all_test_cases(dut, target, suite):
            self.col += 1
            if case[:5] == "test_":
                self.sheet.write(self.row, self.col, case[5:])
            else:
                self.sheet.write(self.row, self.col, case)
            self.__write_result(dut, target, suite, case)
            self.row += 1
            self.col -= 1

    def __write_suites(self, dut, target):
        for suite in self.result.all_test_suites(dut, target):
            self.row += 1
            self.col += 1
            self.sheet.write(self.row, self.col, suite)
            self.__write_cases(dut, target, suite)
            self.col -= 1

    def __write_nic(self, dut, target):
        nic = self.result.current_nic(dut, target)
        self.col += 1
        self.sheet.write(self.row, self.col, nic, self.title_style)
        self.__write_suites(dut, target)
        self.col -= 1

    def __write_failed_target(self, dut, target):
        msg = "TARGET ERROR '%s'" % self.result.target_failed_msg(dut, target)
        self.sheet.write(self.row, self.col + 4, msg, self.failed_style)
        self.row += 1

    def __write_targets(self, dut):
        for target in self.result.all_targets(dut):
            self.col += 1
            self.sheet.write(self.row, self.col, target, self.title_style)
            if self.result.is_target_failed(dut, target):
                self.__write_failed_target(dut, target)
            else:
                self.__write_nic(dut, target)
            self.row += 1
            self.col -= 1

    def __write_failed_dut(self, dut):
        msg = "PREREQ FAILED '%s'" % self.result.dut_failed_msg(dut)
        self.sheet.write(self.row, self.col + 5, msg, self.failed_style)
        self.row += 1

    def __parse_result(self):
        for dut in self.result.all_duts():
            self.sheet.write(self.row, self.col, dut, self.title_style)
            if self.result.is_dut_failed(dut):
                self.__write_failed_dut(dut)
            else:
                self.__write_targets(dut)
            self.row += 1

    def __save_result(self):
        dut_col = self.__get_col_by_title('DUT')[0]
        target_col = self.__get_col_by_title('Target')[0]
        nic_col = self.__get_col_by_title('NIC')[0]
        suite_col = self.__get_col_by_title('Test suite')[0]
        case_col = self.__get_col_by_title('Test case')[0]
        result_col = self.__get_col_by_title('Results')[0]
        for row in range(1, self.rsheet.nrows):
            dutIP = self.rsheet.cell(row, dut_col).value
            target = self.rsheet.cell(row, target_col).value
            nic = self.rsheet.cell(row, nic_col).value
            suite = self.rsheet.cell(row, suite_col).value
            case = self.rsheet.cell(row, case_col).value
            result = self.rsheet.cell(row, result_col).value
            if dutIP is not '':
                self.result.dut = dutIP
            if target is not '':
                self.result.target = target
            if nic is not '':
                self.result.nic = nic
            if suite is not '':
                self.result.test_suite = suite
            if case is not '':
                self.result.test_case = case
            results = result.replace('\'', '').split(' ', 1)
            if 'PASSED' in result:
                self.result.test_case_passed()
            elif 'BLOCKED' in result:
                self.result.test_case_blocked(results[1])
            elif result != '' and case != '':
                self.result.test_case_failed(results[1])
            elif result != '' and target != '':
                self.result.add_failed_target(
                    self.result.dut, target, results[1])

    def save(self, result):
        self.__write_init()
        self.__add_header()
        self.row = 1
        self.col = 0
        self.result = result
        self.__parse_result()
        self.workbook.save(self.filename)

    def load(self):
        self.__read_init()
        self.result = Result()
        self.__save_result()
        return self.result
