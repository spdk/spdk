#!/usr/bin/env bash

set -e

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/scripts/autotest_common.sh

# delay time for apps to start up as primary
app_start=5

function linux_iter_pci {
	lspci -mm -n -D | grep $1 | tr -d '"' | awk -F " " '{print $1}'
}

timing_enter nvme

if [ `uname` = Linux ]; then
	start_stub "-s 2048 -i 0 -m 0xF"
	sleep $app_start
	trap "kill_stub; exit 1" SIGINT SIGTERM ExIT
fi

if [ $RUN_NIGHTLY -eq 1 ]; then
	timing_enter aer
	$testdir/aer/aer
	timing_exit aer

	timing_enter reset
	$testdir/reset/reset -q 64 -w write -s 4096 -t 2
	timing_exit reset
fi

timing_enter identify
$rootdir/examples/nvme/identify/identify -i 0
for bdf in $(linux_iter_pci 0108); do
	$rootdir/examples/nvme/identify/identify -r "trtype:PCIe traddr:${bdf}" -i 0
done
timing_exit identify

timing_enter perf
$rootdir/examples/nvme/perf/perf -q 128 -w read -s 12288 -t 1 -LL -i 0
timing_exit perf

timing_enter reserve
$rootdir/examples/nvme/reserve/reserve
timing_exit reserve

timing_enter hello_world
$rootdir/examples/nvme/hello_world/hello_world
timing_exit

timing_enter sgl
$testdir/sgl/sgl
timing_exit sgl

timing_enter e2edp
$testdir/e2edp/nvme_dp
timing_exit e2edp

timing_enter overhead
$rootdir/test/lib/nvme/overhead/overhead -s 4096 -t 1 -H
timing_exit overhead

timing_enter arbitration
$rootdir/examples/nvme/arbitration/arbitration -t 3 -i 0
timing_exit arbitration

if [ `uname` = Linux ]; then
	trap - SIGINT SIGTERM EXIT
	kill_stub
fi
PLUGIN_DIR=$rootdir/examples/nvme/fio_plugin

if [ -d /usr/src/fio ]; then
	timing_enter fio_plugin
	for bdf in $(linux_iter_pci 0108); do
		# Only test when ASAN is not enabled. If ASAN is enabled, we cannot test.
		if [ $SPDK_RUN_ASAN -eq 0 ]; then
			LD_PRELOAD=$PLUGIN_DIR/fio_plugin /usr/src/fio/fio $PLUGIN_DIR/example_config.fio --filename="trtype=PCIe traddr=${bdf//:/.} ns=1"
		fi
		break
	done

	timing_exit fio_plugin
fi

if [ $(uname -s) = Linux ] && [ $SPDK_TEST_NVME_MULTIPROCESS -eq 1 ]; then
	timing_enter multi_process
	$rootdir/examples/nvme/arbitration/arbitration -i 0 -s 4096 -t 10 -c 0xf &
	pid=$!
	sleep $app_start
	$rootdir/examples/nvme/perf/perf -i 0 -q 1 -w randread -s 4096 -t 10 -c 0x10 &
	sleep 1
	kill -9 $!
	count=0
	while [ $count -le 2 ]; do
		$rootdir/examples/nvme/perf/perf -i 0 -q 1 -w read -s 4096 -t 1 -c 0xf
		count=$(($count + 1))
	done
	count=0
	while [ $count -le 1 ]; do
		core=$((1 << (($count + 4))))
		printf -v hexcore "0x%x" "$core"
		$rootdir/examples/nvme/perf/perf -i 0 -q 128 -w read -s 4096 -t 1 -c $hexcore &
		sleep $app_start
		$rootdir/examples/nvme/identify/identify -i 0 &
		sleep $app_start
		count=$(($count + 1))
	done
	wait $pid
	timing_exit multi_process
fi

timing_exit nvme
