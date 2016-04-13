#!/usr/bin/env bash

set -e

testdir=$(readlink -f $(dirname $0))
rootdir="$testdir/../../.."
source $rootdir/scripts/autotest_common.sh

function linux_iter_pci {
	lspci -mm -n | grep $1 | tr -d '"' | awk -F " " '{print "0000:"$1}'
}

timing_enter nvme

timing_enter unit
$valgrind $testdir/unit/nvme_ns_cmd_c/nvme_ns_cmd_ut
$testdir/unit/nvme_c/nvme_ut
$valgrind $testdir/unit/nvme_qpair_c/nvme_qpair_ut
$valgrind $testdir/unit/nvme_ctrlr_c/nvme_ctrlr_ut
$valgrind $testdir/unit/nvme_ctrlr_cmd_c/nvme_ctrlr_cmd_ut
timing_exit unit

if [ $RUN_NIGHTLY -eq 1 ]; then
	timing_enter aer
	$testdir/aer/aer
	timing_exit aer
fi

timing_enter identify
$rootdir/examples/nvme/identify/identify
timing_exit identify

timing_enter perf
$rootdir/examples/nvme/perf/perf -q 128 -w read -s 12288 -t 1
timing_exit perf

timing_enter reserve
$rootdir/examples/nvme/reserve/reserve
timing_exit reserve

if [ -d /usr/src/fio ]; then
	timing_enter fio_plugin
	for bdf in $(linux_iter_pci 0108); do
		/usr/src/fio/fio $rootdir/examples/nvme/fio_plugin/example_config.fio --filename=${bdf//:/.}/1
		break
	done

	timing_exit fio_plugin
fi

#Now test nvme reset function
timing_enter reset
$testdir/reset/reset -q 64 -w write -s 4096 -t 2
timing_exit reset

timing_enter sgl
$testdir/sgl/sgl
timing_exit sgl

timing_enter e2edp
$testdir/e2edp/nvme_dp
timing_exit e2edp

timing_exit nvme
