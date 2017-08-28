#! /usr/bin/python3

import subprocess
import argparse
import os
import glob
import re

def main(output_dir, repo_dir):
    with open(os.path.join(output_dir, 'coverage.log'), 'w+') as log_file:
        coveragePath = os.path.join(output_dir, '**', 'cov_total.info')
        covfiles = glob.glob(coveragePath, recursive=True)
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
        cov_total = os.path.join(output_dir, 'cov_total.info')
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


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="SPDK Coverage Processor")
    parser.add_argument("-d", "--directory_location", type=str, required=True,
                        help="The location of your build's output directory")
    parser.add_argument("-r", "--repo_directory", type=str, required=True,
                        help="The location of your spdk repository")
    args = parser.parse_args()
    main(args.directory_location, args.repo_directory)
