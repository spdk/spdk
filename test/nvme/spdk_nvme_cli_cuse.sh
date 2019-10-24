#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/scripts/common.sh
source $rootdir/test/common/autotest_common.sh

timing_enter nvme_cli_cuse

NVME_CMD=/usr/local/src/nvme-cli/nvme
rpc_py=$rootdir/scripts/rpc.py

$rootdir/app/spdk_tgt/spdk_tgt -m 0x3 &
spdk_tgt_pid=$!
trap 'kill -9 ${spdk_tgt_pid}; exit 1' SIGINT SIGTERM EXIT

waitforlisten $spdk_tgt_pid

bdf=$(iter_pci_class_code 01 08 02 | head -1)

$rpc_py bdev_nvme_attach_controller -b Nvme0 -t PCIe -a ${bdf}
$rpc_py bdev_nvme_cuse_register -n Nvme0 -p spdk/nvme0

sleep 5

$rpc_py bdev_get_bdevs
$rpc_py bdev_nvme_get_controllers

for ns in $(ls /dev/spdk/nvme?n?); do
	${NVME_CMD} get-ns-id $ns
	${NVME_CMD} id-ns $ns
	${NVME_CMD} list-ns $ns
done

for ctrlr in $(ls /dev/spdk/nvme?); do
	${NVME_CMD} id-ctrl $ctrlr
	${NVME_CMD} list-ctrl $ctrlr
	${NVME_CMD} fw-log $ctrlr
	${NVME_CMD} smart-log $ctrlr
	${NVME_CMD} error-log $ctrlr
	${NVME_CMD} get-feature $ctrlr -f 1 -s 1 -l 100
	${NVME_CMD} get-log $ctrlr -i 1 -l 100
	${NVME_CMD} reset $ctrlr
done

trap - SIGINT SIGTERM EXIT
kill $spdk_tgt_pid

report_test_completion spdk_nvme_cli_cuse
timing_exit nvme_cli_cuse
