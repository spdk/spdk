#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/scripts/common.sh
source $rootdir/test/common/autotest_common.sh

rpc_py=$rootdir/scripts/rpc.py

pci_devs=$($rootdir/app/spdk_lspci/spdk_lspci | grep "NVMe disk behind VMD" | awk '{print $1}')

if [ -z $pci_devs]; then
        echo "Couldn't find any NVMe device behind a VMD."
        exit 1
fi

timing_enter vmd

timing_enter identify
for bdf in $pci_devs; do
	$rootdir/examples/nvme/identify/identify -i 0 -V -r "trtype:PCIe traddr:$bdf"
done
timing_exit identify

timing_enter hello_world
$rootdir/examples/nvme/hello_world/hello_world -V
timing_exit

timing_enter perf
for bdf in $pci_devs; do
	$rootdir/examples/nvme/perf/perf -q 128 -w read -o 12288 -t 1 -LL -i 0 -V -r "trtype:PCIe traddr:$bdf"
done
timing_exit perf

if [ -d /usr/src/fio ]; then
	timing_enter fio_plugin
	PLUGIN_DIR=$rootdir/examples/nvme/fio_plugin
	for bdf in $pci_devs; do
		fio_nvme $testdir/config/config.fio --filename="trtype=PCIe traddr=${bdf//:/.} ns=1"
		report_test_completion "bdev_fio"
	done
	timing_exit fio_plugin
fi

trap "killprocess $svcpid; exit 1" SIGINT SIGTERM EXIT

$rootdir/test/app/bdev_svc/bdev_svc --wait-for-rpc & svcpid=$!
# Wait until bdev_svc starts
waitforlisten $svcpid

$rpc_py enable_vmd -e
$rpc_py start_subsystem_init

for bdf in $pci_devs; do
	$rpc_py construct_nvme_bdev -b NVMe1 -t PCIe -a $bdf
done
killprocess $svcpid

timing_exit vmd
