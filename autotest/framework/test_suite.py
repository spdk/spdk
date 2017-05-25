# BSD LICENSE
#
# Copyright(c) 2010-2016 Intel Corporation. All rights reserved.
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

import ConfigParser
import os
import traceback
import atexit
import copy
import settings
from tester import Tester
from dut import Dut
from serializer import Serializer
from test_case import TestCase
from test_result import Result
from exception import ConfigParseException, VerifyFailure
from logger import getLogger, get_subclasses
import logger
from config import CrbsConf
import sys
reload(sys)
sys.setdefaultencoding('UTF8')
cwd = os.path.dirname(os.path.dirname(__file__))
sys.path.append(cwd + '/tests/lib')
result = None
log_handler = None


def spdk_parse_param(config, section):
    """
    Parse execution file parameters.
    """
    performance = False
    functional = False
    parameters = config.get(section, 'parameters').split(':')
    drivername = config.get(section, 'drivername').split('=')[-1]
    settings.save_global_setting(settings.HOST_DRIVER_SETTING, drivername)
    paramDict = dict()
    for param in parameters:
        (key, _, value) = param.partition('=')
        paramDict[key] = value
    if 'perf' in paramDict and paramDict['perf'] == 'true':
        performance = True
    if 'func' in paramDict and paramDict['func'] == 'true':
        functional = True
    if 'nic_type' not in paramDict:
        paramDict['nic_type'] = 'any'
    settings.save_global_setting(
        settings.HOST_NIC_SETTING, paramDict['nic_type'])
    if performance:
        settings.save_global_setting(settings.PERF_SETTING, 'yes')
    else:
        settings.save_global_setting(settings.PERF_SETTING, 'no')
    if functional:
        settings.save_global_setting(settings.FUNC_SETTING, 'yes')
    else:
        settings.save_global_setting(settings.FUNC_SETTING, 'no')


def spdk_parse_config(config, section):
    """
    Parse execution file configuration.
    """
    duts = [dut_.strip() for dut_ in config.get(section,
                                                'crbs').split(',')]
    targets = [target.strip()
               for target in config.get(section, 'targets').split(',')]
    test_suites = [suite.strip()
                   for suite in config.get(section, 'test_suites').split(',')]
    for suite in test_suites:
        if suite == '':
            test_suites.remove(suite)
    return duts, targets, test_suites


def get_project_obj(project_name, super_class, crbInst, serializer):
    """
    Load project module and return crb instance.
    """
    project_obj = None
    PROJECT_MODULE_PREFIX = 'project_'
    try:
        project_module = __import__(PROJECT_MODULE_PREFIX + project_name)
        for project_subclassname, project_subclass in get_subclasses(
                project_module, super_class):
            project_obj = project_subclass(crbInst, serializer)
        if project_obj is None:
            project_obj = super_class(crbInst, serializer)
    except Exception as e:
        log_handler.info("LOAD PROJECT MODULE INFO: " + str(e))
        project_obj = super_class(crbInst, serializer)
    return project_obj


def spdk_log_testsuite(duts, tester, suite_obj, log_handler, test_classname):
    """
    Change SUITE self logger handler.
    """
    log_handler.config_suite(test_classname, 'spdk')
    tester.logger.config_suite(test_classname, 'tester')
    for dutobj in duts:
        dutobj.logger.config_suite(test_classname, 'dut')
        dutobj.test_classname = test_classname


def spdk_log_execution(duts, tester, log_handler):
    """
    Change default logger handler.
    """
    log_handler.config_execution('spdk')
    tester.logger.config_execution('tester')
    for dutobj in duts:
        dutobj.logger.config_execution(
            'dut' + settings.LOG_NAME_SEP + '%s' % dutobj.crb['My IP'])


def spdk_crbs_init(crbInsts, skip_setup, project,
                   base_dir, serializer, dpdk_dir):
    """
    Create dut/tester instance and initialize them.
    """
    duts = []
    serializer.set_serialized_filename(settings.FOLDERS['Output'] +
                                       '/.%s.cache' % crbInsts[0]['IP'])
    serializer.load_from_file()
    testInst = copy.copy(crbInsts[0])
    testInst['My IP'] = crbInsts[0]['tester IP']
    tester = get_project_obj(project, Tester, testInst, serializer)
    for crbInst in crbInsts:
        dutInst = copy.copy(crbInst)
        dutInst['My IP'] = crbInst['IP']
        dutobj = get_project_obj(project, Dut, dutInst, serializer)
        duts.append(dutobj)
    spdk_log_execution(duts, tester, log_handler)
    tester.duts = duts
    show_speedup_options_messages(skip_setup)
    tester.set_speedup_options(skip_setup)
    nic = settings.load_global_setting(settings.HOST_NIC_SETTING)
    for dutobj in duts:
        dutobj.tester = tester
        dutobj.set_speedup_options(skip_setup)
        dutobj.set_directory(base_dir)
        dutobj.set_path(dpdk_dir)
    return duts, tester


def spdk_crbs_exit(duts, tester):
    """
    Call dut and tester exit function after execution finished
    """
    for dutobj in duts:
        dutobj.crb_exit()
    tester.crb_exit()


def spdk_run_prerequisties(duts, tester, serializer):
    """
    Run spdk prerequisties function.
    """
    try:
        tester.prerequisites()
    except Exception as ex:
        log_handler.error(" PREREQ EXCEPTION " + traceback.format_exc())
        log_handler.info('CACHE: Discarding cache.')
        settings.report_error("TESTER_SETUP_ERR")
        return False
    try:
        for dutobj in duts:
            dutobj.prerequisites()
        serializer.save_to_file()
    except Exception as ex:
        log_handler.error(" PREREQ EXCEPTION " + traceback.format_exc())
        result.add_failed_dut(duts[0], str(ex))
        log_handler.info('CACHE: Discarding cache.')
        settings.report_error("DUT_SETUP_ERR")
        return False


def spdk_run_target(duts, tester, targets, test_suites):
    """
    Run each target in execution targets.
    """
    for target in targets:
        log_handler.info("\nTARGET " + target)
        result.target = target
        try:
            drivername = settings.load_global_setting(
                settings.HOST_DRIVER_SETTING)
            if drivername == "":
                for dutobj in duts:
                    dutobj.set_target(target, bind_dev=False)
            else:
                for dutobj in duts:
                    dutobj.set_target(target)
        except AssertionError as ex:
            log_handler.error(" TARGET ERROR: " + str(ex))
            settings.report_error("SPDK_BUILD_ERR")
            result.add_failed_target(result.dut, target, str(ex))
            continue
        except Exception as ex:
            settings.report_error("GENERIC_ERR")
            log_handler.error(" !!! DEBUG IT: " + traceback.format_exc())
            result.add_failed_target(result.dut, target, str(ex))
            continue
        spdk_run_suite(duts, tester, test_suites, target)


def spdk_run_suite(duts, tester, test_suites, target):
    """
    Run each suite in test suite list.
    """
    for suite_name in test_suites:
        try:
            result.test_suite = suite_name
            suite_module = __import__('TestSuite_' + suite_name)
            for test_classname, test_class in get_subclasses(
                    suite_module, TestCase):
                suite_obj = test_class(duts, tester, target, suite_name)
                result.nic = suite_obj.nic
                spdk_log_testsuite(duts, tester, suite_obj,
                                   log_handler, test_classname)
                log_handler.info("\nTEST SUITE : " + test_classname)
                log_handler.info("NIC :        " + result.nic)
                if suite_obj.execute_setup_all():
                    suite_obj.execute_test_cases()
                    suite_obj.execute_tear_downall()
                log_handler.info("\nTEST SUITE ENDED: " + test_classname)
                spdk_log_execution(duts, tester, log_handler)
        except VerifyFailure:
            settings.report_error("SUITE_EXECUTE_ERR")
            log_handler.error(" !!! DEBUG IT: " + traceback.format_exc())
        except KeyboardInterrupt:
            log_handler.error(" !!! STOPPING SPDK tests")
            suite_obj.execute_tear_downall()
            break
        except Exception as e:
            settings.report_error("GENERIC_ERR")
            log_handler.error(str(e))
        finally:
            suite_obj.execute_tear_downall()


def run_all(config_file, skip_setup, project,
            suite_dir, base_dir, output_dir, dpdk_dir):
    """
    Main process of SPDK tests, it will run all test suites in the config file.
    """
    global result
    global log_handler
    # save global variable
    serializer = Serializer()
    # prepare the output folder
    if output_dir == '':
        output_dir = settings.FOLDERS['Output']
    if not os.path.exists(output_dir):
        os.mkdir(output_dir)
    # add python module search path
    sys.path.append(suite_dir)
    sys.path.append(dpdk_dir)
    logger.log_dir = output_dir
    log_handler = getLogger('spdk')
    log_handler.config_execution('spdk')
    # Read config file
    config = ConfigParser.SafeConfigParser()
    load_cfg = config.read(config_file)
    if len(load_cfg) == 0:
        raise ConfigParseException(config_file)
    os.environ["TERM"] = "dumb"
    # report objects
    result = Result()
    crbInsts = []
    crbs_conf = CrbsConf()
    crbs = crbs_conf.load_crbs_config()
    # for all Exectuion sections
    for section in config.sections():
        spdk_parse_param(config, section)
        # verify if the delimiter is good if the lists are vertical
        dutIPs, targets, test_suites = spdk_parse_config(config, section)
        for dutIP in dutIPs:
            log_handler.info("\nDUT " + dutIP)
        # look up in crbs - to find the matching IP
        for dutIP in dutIPs:
            for crb in crbs:
                if crb['IP'] == dutIP:
                    crbInsts.append(crb)
                    break
        # only run on the dut in known crbs
        if len(crbInsts) == 0:
            cwd = os.path.dirname(os.path.dirname(__file__))
            path1 = cwd + '/framework/execution.cfg'
            path2 = cwd + '/framework/crbs.cfg'
            print "               <Target_IP_Address> is", dutIP, "in", path1
            log_handler.error(" SKIP UNKNOWN TARGET")
            if dutIP != '<Target_IP_Address>':
                print "               Please check IP Address information in", path1, "and", path2
            continue
        result.dut = dutIPs[0]
        # init dut, tester crb
        duts, tester = spdk_crbs_init(
            crbInsts, skip_setup, project, base_dir, serializer, dpdk_dir)
        # register exit action
        atexit.register(quit_execution, duts, tester)
        # Run DUT prerequisites
        if spdk_run_prerequisties(duts, tester, serializer) is False:
            spdk_crbs_exit(duts, tester)
            continue
        spdk_run_target(duts, tester, targets, test_suites)
        spdk_crbs_exit(duts, tester)


def show_speedup_options_messages(skip_setup):
    """
    Skip NIC and spdk setup.
    """
    if skip_setup:
        log_handler.info('SKIP: Skipping SPDK setup.')
    else:
        log_handler.info('SKIP: The SPDK setup steps will be executed.')


def quit_execution(duts, tester):
    """
    Close session to DUT and tester before quit.
    Return exit status when failure occurred.
    """
    # close all nics
    for dutobj in duts:
        if getattr(dutobj, 'ports_info', None) and dutobj.ports_info:
            for port_info in dutobj.ports_info:
                netdev = port_info['port']
                netdev.close()
        # close all session
        dutobj.close()
    if tester is not None:
        tester.close()
    log_handler.info("SPDK tests ended")
    # return value
    settings.exit_error()
