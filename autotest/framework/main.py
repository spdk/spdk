#!/usr/bin/python

import argparse
import test_suite

parser = argparse.ArgumentParser(description="SPDK Nightly Test Framework")

parser.add_argument('--config-file',
                    default='execution.cfg',
                    help='set target ip and nic config and test suites')

parser.add_argument('--output',
                    default='../output',
                    help='save log and result in output directory')

parser.add_argument('-s', '--skip-setup',
                    action='store_true',
                    help='skip config nic and compile spdk')

parser.add_argument('-p', '--project',
                    default='spdk',
                    help='add spdk as test project.')

parser.add_argument('--suite-dir',
                    default='../tests',
                    help='add tests directory to import test suites')

parser.add_argument('--dpdk-dir',
                    default='~/spdk/dpdk',
                    help='Configure spdk where dpdk packages is added')

parser.add_argument('-d', '--dir',
                    default='~/spdk',
                    help='Output directory where spdk package is extracted')

args = parser.parse_args()

test_suite.run_all(
    args.config_file,
    args.skip_setup,
    args.project,
    args.suite_dir,
    args.dir,
    args.output,
    args.dpdk_dir)
