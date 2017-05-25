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

import logging
import os
import inspect
from settings import LOG_NAME_SEP, FOLDERS

"""
logger module with several log level. Testframwork and TestSuite log
will be saved into different log files.
"""

SPDK_ENV_PAT = r"SPDK_*"


def RED(text):
    return "\x1B[" + "31;1m" + str(text) + "\x1B[" + "0m"


def GREEN(text):
    return "\x1B[" + "32;1m" + str(text) + "\x1B[" + "0m"


def get_subclasses(module, clazz):
    """
    Get module attribute name and attribute.
    """
    for subclazz_name, subclazz in inspect.getmembers(module):
        if hasattr(subclazz, '__bases__') and clazz in subclazz.__bases__:
            yield (subclazz_name, subclazz)

logging.SPDK_DUT_CMD = logging.INFO + 1
logging.SPDK_DUT_OUTPUT = logging.DEBUG + 1
logging.SPDK_DUT_RESULT = logging.WARNING + 1
logging.SPDK_TESTER_CMD = logging.INFO + 2
logging.SPDK_TESTER_OUTPUT = logging.DEBUG + 2
logging.SPDK_TESTER_RESULT = logging.WARNING + 2
logging.SUITE_DUT_CMD = logging.INFO + 3
logging.SUITE_DUT_OUTPUT = logging.DEBUG + 3
logging.SUITE_TESTER_CMD = logging.INFO + 4
logging.SUITE_TESTER_OUTPUT = logging.DEBUG + 4

logging.addLevelName(logging.SPDK_DUT_CMD, 'SPDK_DUT_CMD')
logging.addLevelName(logging.SPDK_DUT_OUTPUT, 'SPDK_DUT_OUTPUT')
logging.addLevelName(logging.SPDK_DUT_RESULT, 'SPDK_DUT_RESULT')
logging.addLevelName(logging.SPDK_TESTER_CMD, 'SPDK_TESTER_CMD')
logging.addLevelName(logging.SPDK_TESTER_OUTPUT, 'SPDK_TESTER_OUTPUT')
logging.addLevelName(logging.SPDK_TESTER_RESULT, 'SPDK_TESTER_RESULT')
logging.addLevelName(logging.SUITE_DUT_CMD, 'SUITE_DUT_CMD')
logging.addLevelName(logging.SUITE_DUT_OUTPUT, 'SUITE_DUT_OUTPUT')
logging.addLevelName(logging.SUITE_TESTER_CMD, 'SUITE_TESTER_CMD')
logging.addLevelName(logging.SUITE_TESTER_OUTPUT, 'SUITE_TESTER_OUTPUT')

message_fmt = '%(asctime)s %(levelname)20s: %(message)s'
date_fmt = '%d/%m/%Y %H:%M:%S'
RESET_COLOR = '\033[0m'
stream_fmt = '%(color)s%(levelname)20s: %(message)s' + RESET_COLOR
log_dir = None


def add_salt(salt, msg):
    if not salt:
        return msg
    else:
        return '[%s] ' % salt + str(msg)


class BaseLoggerAdapter(logging.LoggerAdapter):
    """
    Upper layer of original logging module.
    """

    def spdk_dut_cmd(self, msg, *args, **kwargs):
        self.log(logging.SPDK_DUT_CMD, msg, *args, **kwargs)

    def spdk_dut_output(self, msg, *args, **kwargs):
        self.log(logging.SPDK_DUT_OUTPUT, msg, *args, **kwargs)

    def spdk_dut_result(self, msg, *args, **kwargs):
        self.log(logging.SPDK_DUT_RESULT, msg, *args, **kwargs)

    def spdk_tester_cmd(self, msg, *args, **kwargs):
        self.log(logging.SPDK_TESTER_CMD, msg, *args, **kwargs)

    def spdk_tester_output(self, msg, *args, **kwargs):
        self.log(logging.SPDK_TESTER_CMD, msg, *args, **kwargs)

    def spdk_tester_result(self, msg, *args, **kwargs):
        self.log(logging.SPDK_TESTER_RESULT, msg, *args, **kwargs)

    def suite_dut_cmd(self, msg, *args, **kwargs):
        self.log(logging.SUITE_DUT_CMD, msg, *args, **kwargs)

    def suite_dut_output(self, msg, *args, **kwargs):
        self.log(logging.SUITE_DUT_OUTPUT, msg, *args, **kwargs)

    def suite_tester_cmd(self, msg, *args, **kwargs):
        self.log(logging.SUITE_TESTER_CMD, msg, *args, **kwargs)

    def suite_tester_output(self, msg, *args, **kwargs):
        self.log(logging.SUITE_TESTER_OUTPUT, msg, *args, **kwargs)


class ColorHandler(logging.StreamHandler):
    """
    Color of log format.
    """
    LEVEL_COLORS = {
        logging.DEBUG: '',  # SYSTEM
        logging.SPDK_DUT_OUTPUT: '\033[00;37m',  # WHITE
        logging.SPDK_TESTER_OUTPUT: '\033[00;37m',  # WHITE
        logging.SUITE_DUT_OUTPUT: '\033[00;37m',  # WHITE
        logging.SUITE_TESTER_OUTPUT: '\033[00;37m',  # WHITE
        logging.INFO: '\033[00;36m',  # CYAN
        logging.SPDK_DUT_CMD: '',  # SYSTEM
        logging.SPDK_TESTER_CMD: '',  # SYSTEM
        logging.SUITE_DUT_CMD: '',  # SYSTEM
        logging.SUITE_TESTER_CMD: '',  # SYSTEM
        logging.WARN: '\033[01;33m',  # BOLD YELLOW
        logging.SPDK_DUT_RESULT: '\033[01;34m',  # BOLD BLUE
        logging.SPDK_TESTER_RESULT: '\033[01;34m',  # BOLD BLUE
        logging.ERROR: '\033[01;31m',  # BOLD RED
        logging.CRITICAL: '\033[01;31m',  # BOLD RED
    }

    def format(self, record):
        record.__dict__['color'] = self.LEVEL_COLORS[record.levelno]
        return logging.StreamHandler.format(self, record)


class SPDKLOG(BaseLoggerAdapter):
    """
    log class for framework and testsuite.
    """

    def __init__(self, logger, crb="suite"):
        global log_dir
        filename = inspect.stack()[1][1][:-3]
        self.name = filename.split('/')[-1]
        self.error_lvl = logging.ERROR
        self.warn_lvl = logging.WARNING
        self.info_lvl = logging.INFO
        self.debug_lvl = logging.DEBUG
        if log_dir is None:
            self.log_path = os.getcwd() + "/../" + FOLDERS['Output']
        else:
            self.log_path = log_dir    # log dir should contain tag/crb global value and mod in spdk
        self.spdk_log = "TestFramework.log"
        self.logger = logger
        self.logger.setLevel(logging.DEBUG)
        self.crb = crb
        super(SPDKLOG, self).__init__(self.logger, dict(crb=self.crb))
        self.salt = ''
        self.fh = None
        self.ch = None
        # add default log file
        fh = logging.FileHandler(self.log_path + "/" + self.spdk_log)
        ch = ColorHandler()
        self.__log_hander(fh, ch)

    def __log_hander(self, fh, ch):
        """
        Config stream handler and file handler.
        """
        fh.setFormatter(logging.Formatter(message_fmt, date_fmt))
        ch.setFormatter(logging.Formatter(stream_fmt, date_fmt))
        # file hander default level
        fh.setLevel(logging.DEBUG)
        # console handler default leve
        ch.setLevel(logging.INFO)
        self.logger.addHandler(fh)
        self.logger.addHandler(ch)
        if self.fh is not None:
            self.logger.removeHandler(self.fh)
        if self.ch is not None:
            self.logger.removeHandler(self.ch)
        self.fh = fh
        self.ch = ch

    def warning(self, message):
        """
        warning level log function.
        """
        message = add_salt(self.salt, message)
        self.logger.log(self.warn_lvl, message)

    def info(self, message):
        """
        information level log function.
        """
        message = add_salt(self.salt, message)
        self.logger.log(self.info_lvl, message)

    def error(self, message):
        """
        error level log function.
        """
        message = add_salt(self.salt, message)
        self.logger.log(self.error_lvl, message)

    def debug(self, message):
        """
        debug level log function.
        """
        message = add_salt(self.salt, message)
        self.logger.log(self.debug_lvl, message)

    def config_execution(self, crb):
        """
        Reconfigure framework logfile.
        """
        log_file = self.log_path + '/' + self.spdk_log
        fh = logging.FileHandler(log_file)
        ch = ColorHandler()
        self.__log_hander(fh, ch)

        def set_salt(crb, start_flag):
            if LOG_NAME_SEP in crb:
                old = '%s%s' % (start_flag, LOG_NAME_SEP)
                if not self.salt:
                    self.salt = crb.replace(old, '', 1)
        if crb.startswith('dut'):
            self.info_lvl = logging.SPDK_DUT_CMD
            self.debug_lvl = logging.SPDK_DUT_OUTPUT
            self.warn_lvl = logging.SPDK_DUT_RESULT
            set_salt(crb, 'dut')
        elif crb.startswith('tester'):
            self.info_lvl = logging.SPDK_TESTER_CMD
            self.debug_lvl = logging.SPDK_TESTER_OUTPUT
            self.warn_lvl = logging.SPDK_TESTER_RESULT
            set_salt(crb, 'tester')
        else:
            self.error_lvl = logging.ERROR
            self.warn_lvl = logging.WARNING
            self.info_lvl = logging.INFO
            self.debug_lvl = logging.DEBUG

    def config_suite(self, suitename, crb=None):
        """
        Reconfigure suitename logfile.
        """
        log_file = self.log_path + '/' + suitename + '.log'
        fh = logging.FileHandler(log_file)
        ch = ColorHandler()
        self.__log_hander(fh, ch)
        if crb == 'dut':
            self.info_lvl = logging.SUITE_DUT_CMD
            self.debug_lvl = logging.SUITE_DUT_OUTPUT
        elif crb == 'tester':
            self.info_lvl = logging.SUITE_TESTER_CMD
            self.debug_lvl = logging.SUITE_TESTER_OUTPUT


def getLogger(name, crb="suite"):
    """
    Get logger handler and if there's no handler will create one.
    """
    logger = SPDKLOG(logging.getLogger(name), crb)
    return logger

_TESTSUITE_NAME_FORMAT_PATTERN = r'TEST SUITE : (.*)'
_TESTSUITE_ENDED_FORMAT_PATTERN = r'TEST SUITE ENDED: (.*)'
_TESTCASE_NAME_FORMAT_PATTERN = r'Test Case (.*) Begin'
_TESTCASE_RESULT_FORMAT_PATTERN = r'Test Case (.*) Result (.*):'
