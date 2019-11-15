#!/usr/bin/env python3

import os
import re
import sys
import json
import paramiko
import zipfile
import threading
import subprocess
import itertools
import time
import uuid
import rpc
import rpc.client
from common import *
import logging
from prettytable import PrettyTable

script_dir = os.path.dirname(os.path.abspath(sys.argv[0]))
extract_dir = os.path.dirname(script_dir)
sys.path.append(os.path.join(extract_dir, 'common'))
from test_common import *
# from ..common_pkg.reconstruction import *
now = int(round(time.time() * 1000))
timestamp = time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(now / 1000))
timestamp = timestamp.replace(' ', '_')
timestamp = timestamp.replace(':', '_')
result_file = '/home/iscsi_log/reserve_result_' + timestamp + '.txt'
result_dict = {}




def initiator_run_fio(initiator_obj, target_obj, case, type_rw=None):
    target_results_dir = "/tmp/results"
    postfix = case
    if not os.path.exists("/tmp/results"):
        out = subprocess.check_output("mkdir /tmp/results", shell=True).decode(encoding="utf-8")
    if os.path.exists("/tmp/results/" + postfix):
        subprocess.check_output("rm -rf /tmp/results/" + postfix, shell=True).decode(encoding="utf-8")
    subprocess.check_output("mkdir /tmp/results/" + postfix, shell=True).decode(encoding="utf-8")
    target_results_dir = target_results_dir + "/" + postfix
    for block_size, io_depth, rw in fio_workloads:
        block_size = block_size[0]
        io_depth = io_depth[0]
        rw = rw[0]
        if type_rw:
            rw = type_rw
        cfg = initiator_obj.iscsi_fio_config(block_size, io_depth, rw, fio_run_time, fio_num_jobs, 0)
        outcome = initiator_obj.run_fio(cfg, fio_run_num)
        if not outcome:
            return False
        if target_obj.enable_sar:
            sar_file_name = "_".join([str(block_size), str(rw), str(io_depth), "sar"])
            sar_file_name = ".".join([sar_file_name, "txt"])
            initiator_obj.measure_sar(target_results_dir, sar_file_name)
        initiator_obj.copy_result_files(target_results_dir)

    target_obj.parse_results(target_results_dir)
    return True


def judging_results(result, initiator_obj, case, f):
    if result:
        result_dict[case] = "pass"
        initiator_obj.log_print("-----------end the %s :successful -----------" % case)
        f.write("Test result: PASSED\r\n")

    else:
        result_dict[case] = "failed"
        initiator_obj.log_print("-----------end the %s :failed -----------" % case)
        f.write("Test result: FAILED\r\n")

    f.write("-----------the %s End -----------\r\n" % case)
    f.write("\n")


def previously_results(case, f):
    f.write('----------- %s Start -----------\n' % case)
    content = case_info[case]
    for i, j in content.items():
        f.write("%s: %s\n" % (i, j))
    if case == "case_1":
        f.write("initiator-a:  sg_persist --out --register --param-sark=123abc /dev/sdb\n")
        f.write("initiator-a/b/c : sg_persist --in --read-keys --device=/dev/sdb\n")
        f.write("initiator-a/b/c : read keys value should be initiator-a register param-sark:123abc \n")
    elif case == "case_2":
        f.write("initiator-a:  sg_persist --out --register --param-sark=123abc /dev/sdb\n")
        f.write("initiator-a:  sg_persist --out --register-ignore --param-sark=abc123 /dev/sdb\n")
        f.write("initiator-a/b/c : sg_persist --in --read-keys --device=/dev/sdb\n")
        f.write("initiator-a/b/c : read keys value should be initiator-a register-ignore param-sark:abc123 \n")
    elif case == "case_3":
        f.write("initiator-a/b/c:  sg_persist --out --register --param-sark=123abc/456abc/789abc /dev/sdb\n")
        f.write("initiator-a : sg_persist --out --reserve --prout-type=5 --param-rk=123abc /dev/sdb\n")
        f.write("initiator-a : run fio read/write testing to test the iscsi disk, read/write is except ok\n")
    elif case == "case_4":
        f.write("initiator-a/b/c:  sg_persist --out --register --param-sark=123abc/456abc/789abc /dev/sdb\n")
        f.write("initiator-a : sg_persist --out --reserve --prout-type=5 --param-rk=123abc /dev/sdb\n")
        f.write("initiator-b/c: run fio read/write testing to test the iscsi disk, read/write is except ok\n")
    elif case == "case_5":
        f.write("initiator-a/b:  sg_persist --out --register --param-sark=123abc/456abc /dev/sdb\n")
        f.write("initiator-a : sg_persist --out --reserve --prout-type=5 --param-rk=123abc /dev/sdb\n")
        f.write("initiator-c : run fio read/write testing to test the iscsi disk. read ok but write operation should fail.\n")
    elif case == "case_6":
        f.write("initiator-a/b:  sg_persist --out --register --param-sark=123abc/456abc /dev/sdb\n")
        f.write("initiator-a : sg_persist --out --reserve --prout-type=5 --param-rk=123abc /dev/sdb\n")
        f.write(
            "initiator-b : sg_persist --out --reserve --prout-type=5 --param-rk=456abc /dev/sdb --- this step should fail\n")
    elif case == "case_7":
        f.write("initiator-a/b:  sg_persist --out --register --param-sark=123abc/456abc /dev/sdb\n")
        f.write("initiator-a : sg_persist --out --reserve --prout-type=5 --param-rk=123abc /dev/sdb\n")
        f.write(
            "initiator-c : sg_persist --out --reserve --prout-type=5 --param-rk=456abc /dev/sdb --- this step should fail\n")
    elif case == "case_8":
        f.write("initiator-a/b:  sg_persist --out --register --param-sark=123abc/456abc /dev/sdb\n")
        f.write("initiator-a : sg_persist --out --reserve --prout-type=5 --param-rk=123abc /dev/sdb\n")
        f.write(
            "initiator-b : sg_persist --out --release --prout-type=5 --param-rk=456abc /dev/sdb --- this step should pass, but reservation is not released.\n")
    elif case == "case_9":
        f.write("initiator-a/b:  sg_persist --out --register --param-sark=123abc/456abc /dev/sdb\n")
        f.write("initiator-a : sg_persist --out --reserve --prout-type=5 --param-rk=123abc /dev/sdb\n")
        f.write(
            "initiator-c : sg_persist --out --release --prout-type=5 --param-rk=456abc /dev/sdb --- this step should fail, can't release.\n")
    elif case == "case_10":
        f.write("initiator-a/b:  sg_persist --out --register --param-sark=123abc/456abc /dev/sdb\n")
        f.write("initiator-a : sg_persist --out --reserve --prout-type=5 --param-rk=123abc /dev/sdb\n")
        f.write(
            "initiator-a : sg_persist --out --clear --prout-type=5 --param-rk=123abc /dev/sdb\n")
        f.write("initiator-a :  sg_persist --in -k  --device=/dev/sdb  --- this step should be successsful, the keys has cleared\n")
    elif case == "case_11":
        f.write("initiator-a/b:  sg_persist --out --register --param-sark=123abc/456abc /dev/sdb\n")
        f.write("initiator-a : sg_persist --out --reserve --prout-type=5 --param-rk=123abc /dev/sdb\n")
        f.write(
            "initiator-b : sg_persist --out --clear --prout-type=5 --param-rk=123abc /dev/sdb\n")
        f.write(
            "initiator-a :  sg_persist --in -k  --device=/dev/sdb  --- this step should be successsful, the keys has cleared\n")
    elif case == "case_12":
        f.write("initiator-a/b:  sg_persist --out --register --param-sark=123abc/456abc /dev/sdb\n")
        f.write("initiator-a : sg_persist --out --reserve --prout-type=5 --param-rk=123abc /dev/sdb\n")
        f.write(
            "initiator-c : sg_persist --out --clear --prout-type=5 --param-rk=123abc /dev/sdb --- this step should be fail, can't clear the keys\n")
    elif case == "case_13":
        f.write("initiator-a:  sg_persist --out --register --param-sark=123abc /dev/sdb\n")
        f.write("initiator-a : sg_persist --out --reserve --prout-type=5 --param-rk=123abc /dev/sdb\n")
        f.write(
            "initiator-b : sg_persist --out --preempt --prout-type=5 --param-rk=456abc --param-sark=123abc /dev/sdb --- this step should fail, can't preempt\n")
    elif case == "case_14":
        f.write("initiator-a/b:  sg_persist --out --register --param-sark=123abc/456abc /dev/sdb\n")
        f.write("initiator-a : sg_persist --out --reserve --prout-type=5 --param-rk=123abc /dev/sdb\n")
        f.write(
            "initiator-b : sg_persist --out --preempt --prout-type=5 --param-rk=456abc --param-sark=123abc /dev/sdb --- this step should fail, can't preempt\n")
        f.write("initiator-b : run fio write testing to test the iscsi disk. write operation should ok.\n")
    elif case == "case_15":
        f.write("initiator-a/b:  sg_persist --out --register --param-sark=123abc/456abc /dev/sdb\n")
        f.write("initiator-a : sg_persist --out --reserve --prout-type=5 --param-rk=123abc /dev/sdb\n")
        f.write(
            "initiator-b : sg_persist --out --preempt --prout-type=5 --param-rk=456abc --param-sark=123abc /dev/sdb --- this step should fail, can't preempt\n")
        f.write(
            "initiator-b : run fio read/write testing to test the iscsi disk. write operation should ok.\n")
        f.write("initiator-a:  sg_persist --out --register --param-sark=123abc /dev/sdb\n")
        f.write("initiator-a : sg_persist --out --preempt --prout-type=5 --param-rk=123abc --param-sark=456abc /dev/sdb\n")
        f.write(
            "initiator-a : run fio write testing to test the iscsi disk. write operation should ok.\n")
        f.write(
            "initiator-b : run fio write testing to test the iscsi disk. write operation should fail.\n")
    f.write('-------------')
    f.write('\n')


def end_case(res, case):
    f.write('Test result: %s\n' % res)
    f.write('-----------the %s End -----------\n' % case)
    f.write("\r\n")


def case_1(initiators, case):
    result = True
    register_value = "123abc"
    for i, initiator_obj in enumerate(initiators):
        initiator_obj.iscsi_login()
        device_paths = initiator_obj.get_device_path()
        if i + 1 == 1:
            initiator_obj.log_print("*********************************************************************************")
            initiator_obj.log_print("-----------start to test %s-----------" % case)
            previously_results(case, f)
            reg = initiator_obj.reserve_register_key(register_value, device_paths)
            if case == "case_2":
                register_value = "abc123"
                reg = initiator_obj.reserve_register_ignore(register_value, device_paths)
            if reg:
                register_1, keys_1 = initiator_obj.reserve_read_keys(device_paths)
                if not register_1 or not keys_1:
                    initiator_obj.log_print("initiator_a read keys failed")
                    f.write("initiator_a read keys failed\n")
                    result = False
                elif keys_1[0] != register_value:
                    initiator_obj.log_print("initiator_a read keys false")
                    initiator_obj.log_print(
                        "initiator_a read keys is [%s], the true keys is [%s]" % (keys_1[0], register_value))
                    f.write("initiator_a read keys failed\n")
                    result = False
            else:
                initiator_obj.log_print("initiator_a register failed")
                f.write("initiator_a register failed\n")
                result = False
        elif i + 1 == 2:
            register_2, keys_2 = initiator_obj.reserve_read_keys(device_paths)
            if not register_2 or not keys_2:
                initiator_obj.log_print("initiator_b read keys failed")
                f.write("initiator_b read keys failed\n")
                result = False
            elif keys_2[0] != register_value:
                initiator_obj.log_print("initiator_b read keys false")
                f.write("initiator_b read keys failed\n")
                initiator_obj.log_print(
                    "initiator_b read keys is [%s], the true keys is [%s]" % (keys_2[0], register_value))
                result = False
        elif i + 1 == 3:
            register_3, keys_3 = initiator_obj.reserve_read_keys(device_paths)
            if not register_3 or not keys_3:
                initiator_obj.log_print("initiator_c read keys failed")
                f.write("initiator_c read keys failed\n")
                result = False
            elif keys_3[0] != register_value:
                initiator_obj.log_print("initiator_c read keys false")
                f.write("initiator_c read keys failed\n")
                initiator_obj.log_print(
                    "initiator_c read keys is [%s], the true keys is [%s]" % (keys_3[0], register_value))
                result = False
            judging_results(result, initiator_obj, case, f)

            initiator_obj.log_print("*********************************************************************************")
        else:
            pass

    for initiator_obj in initiators:
        iscsi_logout_delete = "iscsiadm -m node --logout && iscsiadm -m node -o delete"
        initiator_obj.remote_call(iscsi_logout_delete)


def case_2(initiators):
    case_1(initiators, "case_2")


def case_3(initiators, target_obj, case):
    result = True
    value_a = ''
    device_a = ''
    for i, initiator_obj in enumerate(initiators):
        initiator_obj.iscsi_login()
        device_paths = initiator_obj.get_device_path()
        register_value = ""
        initiator_name = ""
        if i+1 == 1:
            initiator_obj.log_print("*********************************************************************************")
            initiator_obj.log_print("-----------start to test %s-----------" % case)
            register_value = "123abc"
            initiator_name = "initiator_a"
            value_a = register_value
            device_a = device_paths
            previously_results(case, f)
        elif i+1 == 2:
            register_value = "456abc"
            initiator_name = "initiator_b"
        elif i+1 == 3:
            register_value = "789abc"
            initiator_name = "initiator_c"
        reg = initiator_obj.reserve_register_key(register_value, device_paths)
        if not reg:
            result = False
            initiator_obj.log_print("%s register failed" % initiator_name)
            initiator_obj.log_print("-----------end the %s :failed -----------" % case)
            end_case("FAILED", case)

    if result:
        for i, initiator_obj in enumerate(initiators):
            if i+1 == 1:
                outcome = initiator_obj.reserve_prout_type(5, value_a, device_a)
                if not outcome:
                    result = False
                if case == "case_3":
                    out_come = initiator_run_fio(initiator_obj, target_obj, case)
                    if out_come:
                        initiator_obj.log_print("initiator_a run fio ok")
                        f.write("initiator_a run fio ok\n")
                    else:
                        initiator_obj.log_print("initiator_a run fio failed")
                        f.write("initiator_a run fio failed\n")
                        result = False

                    if not initiator_obj.reserve_clear_prout_type(5, value_a, device_a):
                        result = False
                    judging_results(result, initiator_obj, case, f)
                    break
                else:
                    continue
            elif i+1 == 2:
                out_come = initiator_run_fio(initiator_obj, target_obj, case)
                if out_come:
                    initiator_obj.log_print("initiator_b run fio ok")
                    f.write("initiator_b run fio ok\n")
                else:
                    initiator_obj.log_print("initiator_b run fio failed")
                    f.write("initiator_b run fio failed\n")
                    result = False
            elif i+1 == 3:
                out_come = initiator_run_fio(initiator_obj, target_obj, case)
                if out_come:
                    initiator_obj.log_print("initiator_c run fio ok")
                    f.write("initiator_c run fio ok\n")
                else:
                    initiator_obj.log_print("initiator_c run fio failed")
                    f.write("initiator_c run fio failed\n")
                    result = False
                judging_results(result, initiator_obj, case, f)
            else:
                pass
        if case == "case_4":
            initiators[0].reserve_clear_prout_type(5, value_a, device_a)

    for initiator_obj in initiators:
        iscsi_logout_delete = "iscsiadm -m node --logout && iscsiadm -m node -o delete"
        initiator_obj.remote_call(iscsi_logout_delete)


def case_4(initiators, target_obj):
    case_3(initiators, target_obj, "case_4")


def case_5(initiators, cases, target_obj=None):
    result = True
    fio_read = True
    fio_write = True
    case = cases
    value_a = ''
    device_a = ''
    for i, initiator_obj in enumerate(initiators):
        initiator_obj.iscsi_login()
        device_paths = initiator_obj.get_device_path()
        if i+1 == 1:
            initiator_obj.log_print("*********************************************************************************")
            initiator_obj.log_print("-----------start to test %s-----------" % case)
            previously_results(case, f)
            register_value = "123abc"
            initiator_name = "initiator_a"
            value_a = register_value
            device_a = device_paths
            reg = initiator_obj.reserve_register_key(register_value, device_paths)
            if not reg:
                result = False
                initiator_obj.log_print("%s register failed" % initiator_name)
                f.write("%s register failed\n" % initiator_name)
            outcome = initiator_obj.reserve_prout_type(5, register_value, device_paths)
            if not outcome:
                result = False
                f.write("%s reserve_prout_type=5 failed\n" % initiator_name)
            if case == "case_10":
                outcome = initiator_obj.reserve_clear_prout_type(5, value_a, device_a)
                if outcome:
                    out_come = initiator_obj.reserve_query_prout_type(device_paths)
                if not outcome:
                    result = False
                    f.write("%s sg_persist --in -k  failed\n" % initiator_name)
                judging_results(result, initiator_obj, case, f)
                break
        elif i+1 == 2:
            register_value = "456abc"
            initiator_name = "initiator_b"
            reg = initiator_obj.reserve_register_key(register_value, device_paths)
            if not reg:
                result = False
                initiator_obj.log_print("%s register failed" % initiator_name)
                f.write("%s register failed\n" % initiator_name)
            if case == "case_6":
                outcome = initiator_obj.reserve_prout_type(5, register_value, device_paths)
                if outcome:
                    result = False
                    f.write("%s reserve_prout_type=5 ok\n" % initiator_name)
                judging_results(result, initiator_obj, case, f)
            elif case == "case_8":
                outcome = initiator_obj.reserve_release_prout_type(5, register_value, device_paths)
                if not outcome:
                    result = False
                    f.write("%s release_prout_type failed\n" % initiator_name)
                judging_results(result, initiator_obj, case, f)
            elif case == "case_11":
                outcome = initiator_obj.reserve_clear_prout_type(5, register_value, device_paths)
                if not outcome:
                    result = False
                    f.write("%s clear_prout_type failed\n" % initiator_name)
        elif i+1 == 3:
            register_value = "789abc"
            if case == "case_5":
                out_come = initiator_run_fio(initiator_obj, target_obj, case, "read")
                if out_come:
                    initiator_obj.log_print("initiator_c run fio [read] ok")
                    f.write("initiator_c run fio [read] ok\n")
                else:
                    initiator_obj.log_print("initiator_c run fio [read] failed")
                    fio_read = False
                    f.write("initiator_c run fio [read] failed\n")
                outcome = initiator_run_fio(initiator_obj, target_obj, case, "write")
                if outcome:
                    initiator_obj.log_print("initiator_c run fio [write] ok")
                    f.write("initiator_c run fio [write] ok\n")
                else:
                    initiator_obj.log_print("initiator_c run fio [write] failed")
                    fio_write = False
                    f.write("initiator_c run fio [write] failed\n")
                if result:
                    if fio_read and not fio_write:
                        result_dict[case] = "pass"
                        initiator_obj.log_print("-----------end the %s :successful -----------" % case)
                        end_case("PASSED", case)
                    else:
                        result_dict[case] = "failed"
                        initiator_obj.log_print("-----------end the %s :failed -----------" % case)
                        end_case("FAILED", case)
                else:
                    result_dict[case] = "failed"
                    initiator_obj.log_print("-----------end the %s :failed -----------" % case)
                    end_case("FAILED", case)
            elif case == "case_7":
                outcome = initiator_obj.reserve_prout_type(5, register_value, device_paths)
                if outcome:
                    result = False
                    f.write("initiator_c reserve_prout_type=5 ok\n")
                judging_results(result, initiator_obj, case, f)
            elif case == "case_9":
                outcome = initiator_obj.reserve_release_prout_type(5, register_value, device_paths)
                if outcome:
                    result = False
                    f.write("initiator_c release_prout_type ok\n")
                judging_results(result, initiator_obj, case, f)
            elif case == "case_12":
                outcome = initiator_obj.reserve_clear_prout_type(5, register_value, device_paths)
                if outcome:
                    result = False
                    f.write("initiator_c clear_prout_type ok\n")
                judging_results(result, initiator_obj, case, f)
            else:
                pass
        else:
            pass
    if case == "case_11":
        outcome = initiators[0].reserve_query_prout_type(device_a)
        if not outcome:
            result = False
            f.write("initiator_a sg_persist --in -k  failed\n")
        judging_results(result, initiators[0], case, f)
    if case != "case_10" or case != "case_11":
        initiators[0].reserve_clear_prout_type(5, value_a, device_a)
    for initiator_obj in initiators:
        iscsi_logout_delete = "iscsiadm -m node --logout && iscsiadm -m node -o delete"
        initiator_obj.remote_call(iscsi_logout_delete)


def case_13(initiators, cases, target_obj=None):
    result = True
    case = cases
    value_a = ''
    device_a = ''
    value_b = ''
    for i, initiator_obj in enumerate(initiators):
        initiator_obj.iscsi_login()
        device_paths = initiator_obj.get_device_path()
        if i+1 == 1:
            initiator_obj.log_print("*********************************************************************************")
            initiator_obj.log_print("-----------start to test %s-----------" % case)
            previously_results(case, f)
            register_value = "123abc"
            initiator_name = "initiator_a"
            value_a = register_value
            device_a = device_paths
            reg = initiator_obj.reserve_register_key(register_value, device_paths)
            if not reg:
                result = False
                initiator_obj.log_print("%s register failed" % initiator_name)
                f.write("%s register failed\n" % initiator_name)
            outcome = initiator_obj.reserve_prout_type(5, register_value, device_paths)
            if not outcome:
                result = False
                f.write("%s reserve_prout_type=5 failed\n" % initiator_name)
        elif i+1 == 2:
            register_value = "456abc"
            initiator_name = "initiator_b"
            value_b = register_value
            device_b = device_paths
            if case == "case_13":
                outcome = initiator_obj.reserve_preempt_prout_type(5, register_value, value_a, device_paths)
                if outcome:
                    result = False
                    f.write("%s reserve_preempt_prout_type ok\n" % initiator_name)
                judging_results(result, initiator_obj, case, f)
            elif case == "case_14" or case == "case_15":
                reg = initiator_obj.reserve_register_key(register_value, device_paths)
                if not reg:
                    result = False
                    initiator_obj.log_print("%s register failed" % initiator_name)
                    f.write("%s register failed\n" % initiator_name)
                outcome = initiator_obj.reserve_preempt_prout_type(5, register_value, value_a, device_paths)
                if not outcome:
                    result = False
                    f.write("%s reserve_preempt_prout_type failed\n" % initiator_name)
                outcome = initiator_run_fio(initiator_obj, target_obj, case, "write")
                if outcome:
                    initiator_obj.log_print("initiator_b run fio [write] ok")
                    f.write("initiator_b run fio [write] ok\n")
                else:
                    initiator_obj.log_print("initiator_b run fio [write] failed")
                    result = False
                    f.write("initiator_b run fio [write] failed\n")
                if case == "case_14":
                    judging_results(result, initiator_obj, case, f)
                    initiator_obj.reserve_clear_prout_type(5, value_b, device_b)
                else:
                    if not result:
                        result_dict[case] = "failed"
                        initiator_obj.log_print("-----------end the %s :failed -----------" % case)
                        end_case("FAILED", case)
        else:
            pass
    if result:
        if case == "case_15":
            for i, initiator_obj in enumerate(initiators):
                initiator_obj.iscsi_login()
                device_paths = initiator_obj.get_device_path()
                if i+1 == 1:
                    initiator_name = "initiator_a"
                    reg = initiator_obj.reserve_register_key(value_a, device_paths)
                    if not reg:
                        result = False
                        initiator_obj.log_print("%s register failed" % initiator_name)
                        f.write("%s register failed\n" % initiator_name)
                    outcome = initiator_obj.reserve_preempt_prout_type(5, value_a, value_b, device_paths)
                    if not outcome:
                        result = False
                        f.write("%s reserve_preempt_prout_type failed\n" % initiator_name)
                    outcome = initiator_run_fio(initiator_obj, target_obj, case, "write")
                    if outcome:
                        initiator_obj.log_print("initiator_a run fio [write] ok")
                        f.write("initiator_a run fio [write] ok\n")
                    else:
                        initiator_obj.log_print("initiator_a run fio [write] failed")
                        result = False
                        f.write("initiator_a run fio [write] failed\n")
                elif i+1 == 2:
                    outcome = initiator_run_fio(initiator_obj, target_obj, case, "write")
                    if outcome:
                        initiator_obj.log_print("initiator_b run fio [write] ok")
                        result = False
                        f.write("initiator_b run fio [write] ok\n")
                    else:
                        initiator_obj.log_print("initiator_b run fio [write] failed")
                        f.write("initiator_b run fio [write] failed\n")
                    judging_results(result, initiator_obj, case, f)
                else:
                    pass
    initiators[0].reserve_clear_prout_type(5, value_a, device_a)
    for initiator_obj in initiators:
        iscsi_logout_delete = "iscsiadm -m node --logout && iscsiadm -m node -o delete"
        initiator_obj.remote_call(iscsi_logout_delete)



def case_6(initiators):
    case_5(initiators, "case_6")


def case_7(initiators):
    case_5(initiators, "case_7")


def case_8(initiators):
    case_5(initiators, "case_8")


def case_9(initiators):
    case_5(initiators, "case_9")


def case_10(initiators):
    case_5(initiators, "case_10")


def case_11(initiators):
    case_5(initiators, "case_11")


def case_12(initiators):
    case_5(initiators, "case_12")


def case_14(initiators, target_obj):
    case_13(initiators, "case_14", target_obj)


def case_15(initiators, target_obj):
    case_13(initiators, "case_15", target_obj)



if __name__ == "__main__":
    spdk_zip_path = "/tmp/spdk.zip"
    run_no_case = 0
    if (len(sys.argv) > 1):
        config_file_path = sys.argv[1]
    else:
        script_full_dir = os.path.dirname(os.path.realpath(__file__))
        config_file_path = os.path.join(script_full_dir, "config.json")

    print("Using config file: %s" % config_file_path)
    with open(config_file_path, "r") as config:
        data = json.load(config)
    script_full_dir = os.path.dirname(os.path.realpath(__file__))

    log_name = 'run_iscsi_' + timestamp + '.log'
    log_file_name = '/home/iscsi_log/' + log_name

    initiators = []
    fio_cases = []
    iti_nic_ips_list = []
    fio_workloads = []

    for k, v in data.items():
        if "target" in k:
            if data[k]["mode"] == "spdk":
                target_obj = SPDKTarget(name=k, **data["general"], **v, log_file_name=log_file_name)
        elif "initiator" in k:
            if data[k]["mode"] == "spdk":
                init_obj = SPDKInitiator(name=k, **data["general"], **v, log_file_name=log_file_name)
            initiators.append(init_obj)
        elif "fio" in k:
            print(data[k]["bs"], data[k]["qd"], data[k]["rw"], type(data[k]["bs"]), type(data[k]["qd"]), type(data[k]["rw"]))
            # fio_workloads = itertools.product(data[k]["bs"],
            #                                   data[k]["qd"],
            #                                   data[k]["rw"])
            fio_workloads = [(data[k]["bs"], data[k]["qd"], data[k]["rw"])]

            fio_rw_mix_read = data[k]["rwmixread"]
            fio_run_time = data[k]["run_time"] if "run_time" in data[k].keys() else 10
            fio_ramp_time = data[k]["ramp_time"] if "ramp_time" in data[k].keys() else 0
            fio_run_num = data[k]["run_num"] if "run_num" in data[k].keys() else None
            fio_num_jobs = data[k]["num_jobs"] if "num_jobs" in data[k].keys() else None
            fio_mem_size_mb = data[k]["mem_size_mb"] if "mem_size_mb" in data[k].keys() else None
            fio_size = data[k]["size"] if "size" in data[k].keys() else "8G"
            if "run_no_case" in data[k].keys():
                run_no_case = data[k]["run_no_case"]
        else:
            continue

    # Copy and install SPDK on remote initiators
    # target_obj.zip_spdk_sources(target_obj.spdk_dir, spdk_zip_path)
    # threads = []
    # for i in initiators:
    #     if i.mode == "spdk":
    #         t = threading.Thread(target=i.install_spdk, args=(spdk_zip_path,))
    #         threads.append(t)
    #         t.start()
    # for t in threads:
    #     t.join()

    target_obj.tgt_start(len(initiators))
    f = open(result_file, 'a')
    try:
        if run_no_case:
            table = PrettyTable(['Initiator', 'Block Size', 'fio runtime', 'Test result'])
            for initiator_obj in initiators:
                initiator_obj.iscsi_login()
                out_come = initiator_run_fio(initiator_obj, target_obj, "nocase")
                if out_come:
                    initiator_obj.log_print("initiator_a run fio ok")
                    f.write("[%s] run fio ok\n" % initiator_obj.name)
                    table.add_row([initiator_obj.name, fio_workloads[0][0], fio_workloads[0][2], 'PASSED'])
                else:
                    initiator_obj.log_print("initiator_a run fio failed")
                    f.write("[%s] run fio failed\n" % initiator_obj.name)
                    table.add_row([initiator_obj.name, fio_workloads[0][0], fio_workloads[0][2], 'FAILED'])
                iscsi_logout_delete = "iscsiadm -m node --logout && iscsiadm -m node -o delete"
                initiator_obj.remote_call(iscsi_logout_delete)
            summed = table.get_string().encode()
            f.close()
            time.sleep(5)
            with open(result_file, 'ab') as fl:
                fl.write(summed)
                fl.write(b'\n')
        else:
            with open(os.path.join(script_full_dir, "caseinfo.json")) as fy:
                case_info = fy.read()
                case_info = eval(case_info)
            case_1(initiators, "case_1")
            case_2(initiators)
            case_3(initiators, target_obj, "case_3")
            case_4(initiators, target_obj)
            case_5(initiators, "case_5", target_obj)
            case_6(initiators)
            case_7(initiators)
            case_8(initiators)
            case_9(initiators)
            case_10(initiators)
            case_11(initiators)
            case_12(initiators)
            case_13(initiators, "case_13")
            case_14(initiators, target_obj)
            case_15(initiators, target_obj)
            # json_str = json.dumps(result_dict, indent=1)
            # f.write(json_str)
            table = PrettyTable(['case#', 'Test name', 'Test category', 'Test type', 'Test result'])
            for key, value in case_info.items():
                value['Test result'] = result_dict[key]
                table.add_row([key, value['Test name'], value['Test category'], value['Test type'], value['Test result']])
            summed = table.get_string().encode()
            f.close()
            time.sleep(5)
            with open(result_file, 'ab') as fl:
                fl.write(summed)

    except BaseException as e:
        print(e)
        f.close()
        sys.exit(1)
