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
    data_table = pd.DataFrame(completion_table, columns=["Agent", "Domain", "Test", "With Asan", "With UBsan"])
    data_table.to_html(os.path.join(output_dir, 'completions_table.html'))
    os.makedirs(os.path.join(output_dir, "post_process"), exist_ok=True)

    pivot_by_agent = pd.pivot_table(data_table, index=["Agent", "Domain", "Test"])
    pivot_by_agent.to_html(os.path.join(output_dir, "post_process", 'completions_table_by_agent.html'))
    pivot_by_test = pd.pivot_table(data_table, index=["Domain", "Test", "Agent"])
    pivot_by_test.to_html(os.path.join(output_dir, "post_process", 'completions_table_by_test.html'))
    pivot_by_asan = pd.pivot_table(data_table, index=["Domain", "Test"], values=["With Asan"], aggfunc=highest_value)
    pivot_by_asan.to_html(os.path.join(output_dir, "post_process", 'completions_table_by_asan.html'))
    pivot_by_ubsan = pd.pivot_table(data_table, index=["Domain", "Test"], values=["With UBsan"], aggfunc=highest_value)
    pivot_by_ubsan.to_html(os.path.join(output_dir, "post_process", 'completions_table_by_ubsan.html'))


def generateCoverageReport(output_dir, repo_dir):
    coveragePath = os.path.join(output_dir, '**', 'cov_total.info')
    covfiles = [os.path.abspath(p) for p in glob.glob(coveragePath, recursive=True)]
    for f in covfiles:
        print(f)
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
        subprocess.check_call([lcov], shell=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    except subprocess.CalledProcessError as e:
        print("lcov failed")
        print(e)
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
        subprocess.check_call([genhtml], shell=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    except subprocess.CalledProcessError as e:
        print("genhtml failed")
        print(e)
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


def getCompletions(completionFile, test_list, test_completion_table):
    agent_name = os.path.basename(os.path.dirname(completionFile))
    with open(completionFile, 'r') as completionList:
        completions = completionList.read()

    asan_enabled = "asan" in completions
    ubsan_enabled = "ubsan" in completions

    for line in completions.splitlines():
        try:
            domain, test_name = line.strip().split()
            test_list[test_name] = (True, asan_enabled | test_list[test_name][1], ubsan_enabled | test_list[test_name][2])
            test_completion_table.append([agent_name, domain, test_name, asan_enabled, ubsan_enabled])
            try:
                test_completion_table.remove(["None", "None", test_name, False, False])
            except ValueError:
                continue
        except KeyError:
            continue


def printList(header, test_list, index, condition):
    print("\n\n-----%s------" % header)
    executed_tests = [x for x in sorted(test_list) if test_list[x][index] is condition]
    print(*executed_tests, sep="\n")


def printListInformation(table_type, test_list):
    printList("%s Executed in Build" % table_type, test_list, 0, True)
    printList("%s Missing From Build" % table_type, test_list, 0, False)
    printList("%s Missing ASAN" % table_type, test_list, 1, False)
    printList("%s Missing UBSAN" % table_type, test_list, 2, False)


def getSkippedTests(repo_dir):
    skipped_test_file = os.path.join(repo_dir, "test", "common", "skipped_tests.txt")
    if not os.path.exists(skipped_test_file):
        return []
    else:
        with open(skipped_test_file, "r") as skipped_test_data:
            return [x.strip() for x in skipped_test_data.readlines() if "#" not in x and x.strip() != '']


def confirmPerPatchTests(test_list, skiplist):
    missing_tests = [x for x in sorted(test_list) if test_list[x][0] is False
                     and x not in skiplist]
    if len(missing_tests) > 0:
        print("Not all tests were run. Failing the build.")
        print(missing_tests)
        exit(1)


def aggregateCompletedTests(output_dir, repo_dir, skip_confirm=False):
    test_list = {}
    test_completion_table = []

    testFiles = glob.glob(os.path.join(output_dir, '**', 'all_tests.txt'), recursive=True)
    completionFiles = glob.glob(os.path.join(output_dir, '**', 'test_completions.txt'), recursive=True)

    if len(testFiles) == 0:
        print("Unable to perform test completion aggregator. No input files.")
        return 0

    with open(testFiles[0], 'r') as raw_test_list:
        for line in raw_test_list:
            try:
                test_name = line.strip()
            except Exception:
                print("Failed to parse a test type.")
                return 1

            test_list[test_name] = (False, False, False)
            test_completion_table.append(["None", "None", test_name, False, False])

    for completionFile in completionFiles:
        getCompletions(completionFile, test_list, test_completion_table)

    printListInformation("Tests", test_list)
    generateTestCompletionTables(output_dir, test_completion_table)
    skipped_tests = getSkippedTests(repo_dir)
    if not skip_confirm:
        confirmPerPatchTests(test_list, skipped_tests)


def main(output_dir, repo_dir, skip_confirm=False):
    print("-----Begin Post Process Script------")
    generateCoverageReport(output_dir, repo_dir)
    collectOne(output_dir, 'doc')
    collectOne(output_dir, 'ut_coverage')
    aggregateCompletedTests(output_dir, repo_dir, skip_confirm)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="SPDK Coverage Processor")
    parser.add_argument("-d", "--directory_location", type=str, required=True,
                        help="The location of your build's output directory")
    parser.add_argument("-r", "--repo_directory", type=str, required=True,
                        help="The location of your spdk repository")
    parser.add_argument("-s", "--skip_confirm", required=False, action="store_true",
                        help="Do not check if all autotest.sh tests were executed.")
    args = parser.parse_args()
    main(args.directory_location, args.repo_directory, args.skip_confirm)
