#!/usr/bin/python3

import shutil
import subprocess
import argparse
import os
import glob
import re
import pandas as pd


def highest_value(inp):
    ret_value = False
    for x in inp:
        if x:
            return True
    else:
        return False


def generateTestCompletionTables(output_dir, completion_table):
    data_table = pd.DataFrame(completion_table, columns=["Agent", "Test", "With Asan", "With UBsan"])
    data_table.to_html(os.path.join(output_dir, 'completions_table.html'))
    os.makedirs(os.path.join(output_dir, "post_process"), exist_ok=True)

    pivot_by_agent = pd.pivot_table(data_table, index=["Agent", "Test"])
    pivot_by_agent.to_html(os.path.join(output_dir, "post_process", 'completions_table_by_agent.html'))
    pivot_by_test = pd.pivot_table(data_table, index=["Test", "Agent"])
    pivot_by_test.to_html(os.path.join(output_dir, "post_process", 'completions_table_by_test.html'))
    pivot_by_asan = pd.pivot_table(data_table, index=["Test"], values=["With Asan"], aggfunc=highest_value)
    pivot_by_asan.to_html(os.path.join(output_dir, "post_process", 'completions_table_by_asan.html'))
    pivot_by_ubsan = pd.pivot_table(data_table, index=["Test"], values=["With UBsan"], aggfunc=highest_value)
    pivot_by_ubsan.to_html(os.path.join(output_dir, "post_process", 'completions_table_by_ubsan.html'))


def generateCoverageReport(output_dir, repo_dir):
    with open(os.path.join(output_dir, 'coverage.log'), 'w+') as log_file:
        coveragePath = os.path.join(output_dir, '**', 'cov_total.info')
        covfiles = [os.path.abspath(p) for p in glob.glob(coveragePath, recursive=True)]
        for f in covfiles:
            print(f, file=log_file)
        if len(covfiles) == 0:
            return
        lcov_opts = [
            '--rc lcov_branch_coverage=1',
            '--rc lcov_function_coverage=1',
            '--rc genhtml_branch_coverage=1',
            '--rc genhtml_function_coverage=1',
            '--rc genhtml_legend=1',
            '--rc geninfo_all_blocks=1',
        ]
        cov_total = os.path.abspath(os.path.join(output_dir, 'cov_total.info'))
        coverage = os.path.join(output_dir, 'coverage')
        lcov = 'lcov' + ' ' + ' '.join(lcov_opts) + ' -q -a ' + ' -a '.join(covfiles) + ' -o ' + cov_total
        genhtml = 'genhtml' + ' ' + ' '.join(lcov_opts) + ' -q ' + cov_total + ' --legend' + ' -t "Combined" --show-details -o ' + coverage
        try:
            subprocess.check_call([lcov], shell=True, stdout=log_file, stderr=log_file)
        except subprocess.CalledProcessError as e:
            print("lcov failed", file=log_file)
            print(e, file=log_file)
            return
        cov_total_file = open(cov_total, 'r')
        replacement = "SF:" + repo_dir
        file_contents = cov_total_file.readlines()
        cov_total_file.close()
        os.remove(cov_total)
        with open(cov_total, 'w+') as file:
            for Line in file_contents:
                Line = re.sub("^SF:.*/repo", replacement, Line)
                file.write(Line + '\n')
        try:
            subprocess.check_call([genhtml], shell=True, stdout=log_file, stderr=log_file)
        except subprocess.CalledProcessError as e:
            print("genhtml failed", file=log_file)
            print(e, file=log_file)
        for f in covfiles:
            os.remove(f)


def collectOne(output_dir, dir_name):
    dirs = glob.glob(os.path.join(output_dir, '*', dir_name))
    dirs.sort()
    if len(dirs) == 0:
        return

    # Collect first instance of dir_name and move it to the top level
    collect_dir = dirs.pop(0)
    shutil.move(collect_dir, os.path.join(output_dir, dir_name))

    # Delete all other instances
    for d in dirs:
        shutil.rmtree(d)


def aggregateCompletedTests(output_dir, repo_dir):
    test_list = {}
    test_with_asan = {}
    test_with_ubsan = {}
    test_completion_table = []
    asan_enabled = False
    ubsan_enabled = False
    test_unit_with_valgrind = False
    testFilePath = os.path.join(output_dir, '**', 'all_tests.txt')
    completionFilePath = os.path.join(output_dir, '**', 'test_completions.txt')
    testFiles = glob.glob(testFilePath, recursive=True)
    completionFiles = glob.glob(completionFilePath, recursive=True)
    testSummary = os.path.join(output_dir, "test_execution.log")

    if len(testFiles) == 0:
        print("Unable to perform test completion aggregator. No input files.")
        return 0
    item = testFiles[0]
    with open(item, 'r') as raw_test_list:
        for line in raw_test_list:
            test_list[line.strip()] = (False, False, False)
            test_completion_table.append(["None", line.strip(), False, False])
    for item in completionFiles:
        agent_name = os.path.split(os.path.split(item)[0])[1]
        with open(item, 'r') as completion_list:
            completions = completion_list.read()

            if "asan" not in completions:
                asan_enabled = False
            else:
                asan_enabled = True

            if "ubsan" not in completions:
                ubsan_enabled = False
            else:
                ubsan_enabled = True

            if "valgrind" in completions and "unittest" in completions:
                test_unit_with_valgrind = True
                test_completion_table.append([agent_name, "valgrind", asan_enabled, ubsan_enabled])
            for line in completions.split('\n'):
                try:
                    test_list[line.strip()] = (True, asan_enabled | test_list[line.strip()][1], ubsan_enabled | test_list[line.strip()][1])
                    test_completion_table.append([agent_name, line.strip(), asan_enabled, ubsan_enabled])
                    try:
                        test_completion_table.remove(["None", line.strip(), False, False])
                    except ValueError:
                        continue
                except KeyError:
                    continue

    with open(testSummary, 'w') as fh:
        fh.write("\n\n-----Tests Executed in Build------\n")
        for item in sorted(test_list):
            if test_list[item][0]:
                fh.write(item + "\n")

        fh.write("\n\n-----Tests Missing From Build------\n")
        if not test_unit_with_valgrind:
            fh.write("UNITTEST_WITH_VALGRIND\n")
        for item in sorted(test_list):
            if test_list[item][0] is False:
                fh.write(item + "\n")

        fh.write("\n\n-----Tests Missing ASAN------\n")
        for item in sorted(test_list):
            if test_list[item][1] is False:
                fh.write(item + "\n")

        fh.write("\n\n-----Tests Missing UBSAN------\n")
        for item in sorted(test_list):
            if test_list[item][2] is False:
                fh.write(item + "\n")

    with open(testSummary, 'r') as fh:
        print(fh.read())

    generateTestCompletionTables(output_dir, test_completion_table)


def main(output_dir, repo_dir):
    generateCoverageReport(output_dir, repo_dir)
    collectOne(output_dir, 'doc')
    collectOne(output_dir, 'ut_coverage')
    aggregateCompletedTests(output_dir, repo_dir)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="SPDK Coverage Processor")
    parser.add_argument("-d", "--directory_location", type=str, required=True,
                        help="The location of your build's output directory")
    parser.add_argument("-r", "--repo_directory", type=str, required=True,
                        help="The location of your spdk repository")
    args = parser.parse_args()
    main(args.directory_location, args.repo_directory)
