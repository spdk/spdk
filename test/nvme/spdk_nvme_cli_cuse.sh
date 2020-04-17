#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/scripts/common.sh
source $rootdir/test/common/autotest_common.sh

rm -Rf $testdir/match_files
mkdir $testdir/match_files

KERNEL_OUT=$testdir/match_files/kernel.out
CUSE_OUT=$testdir/match_files/cuse.out

NVME_CMD=/usr/local/src/nvme-cli/nvme
rpc_py=$rootdir/scripts/rpc.py

bdf=$(get_first_nvme_bdf)

PCI_WHITELIST="${bdf}" $rootdir/scripts/setup.sh reset
sleep 1
bdf_sysfs_path=$( readlink -f /sys/class/nvme/nvme* | grep "$bdf/nvme/nvme" )
if [ -z "$bdf_sysfs_path" ]; then
	echo "setup.sh failed bind kernel driver to ${bdf}"
	return 1
fi
nvme_name=$( basename $bdf_sysfs_path )

set +e

ctrlr="/dev/${nvme_name}"
ns="/dev/${nvme_name}n1"

oacs=$( ${NVME_CMD} id-ctrl $ctrlr | grep oacs | cut -d: -f2 )
oacs_firmware=$(( oacs & 0x4 ))

${NVME_CMD} get-ns-id $ns > ${KERNEL_OUT}.1
${NVME_CMD} id-ns $ns > ${KERNEL_OUT}.2
${NVME_CMD} list-ns $ns > ${KERNEL_OUT}.3

${NVME_CMD} id-ctrl $ctrlr > ${KERNEL_OUT}.4
${NVME_CMD} list-ctrl $ctrlr > ${KERNEL_OUT}.5
if [ "$oacs_firmware" -ne "0" ]; then
	${NVME_CMD} fw-log $ctrlr > ${KERNEL_OUT}.6
fi
${NVME_CMD} smart-log $ctrlr
${NVME_CMD} error-log $ctrlr > ${KERNEL_OUT}.7
${NVME_CMD} get-feature $ctrlr -f 1 -s 1 -l 100 > ${KERNEL_OUT}.8
${NVME_CMD} get-log $ctrlr -i 1 -l 100 > ${KERNEL_OUT}.9
${NVME_CMD} reset $ctrlr > ${KERNEL_OUT}.10

set -e

$rootdir/scripts/setup.sh

$rootdir/app/spdk_tgt/spdk_tgt -m 0x3 &
spdk_tgt_pid=$!
trap 'kill -9 ${spdk_tgt_pid}; exit 1' SIGINT SIGTERM EXIT

waitforlisten $spdk_tgt_pid

$rpc_py bdev_nvme_attach_controller -b Nvme0 -t PCIe -a ${bdf}
$rpc_py bdev_nvme_cuse_register -n Nvme0

sleep 5

if [ ! -c /dev/spdk/nvme0 ]; then
	return 1
fi

$rpc_py bdev_get_bdevs
$rpc_py bdev_nvme_get_controllers

set +e

ns="/dev/spdk/nvme0n1"
${NVME_CMD} get-ns-id $ns > ${CUSE_OUT}.1
${NVME_CMD} id-ns $ns > ${CUSE_OUT}.2
${NVME_CMD} list-ns $ns > ${CUSE_OUT}.3

ctrlr="/dev/spdk/nvme0"
${NVME_CMD} id-ctrl $ctrlr > ${CUSE_OUT}.4
${NVME_CMD} list-ctrl $ctrlr > ${CUSE_OUT}.5
if [ "$oacs_firmware" -ne "0" ]; then
	${NVME_CMD} fw-log $ctrlr > ${CUSE_OUT}.6
fi
${NVME_CMD} smart-log $ctrlr
${NVME_CMD} error-log $ctrlr > ${CUSE_OUT}.7
${NVME_CMD} get-feature $ctrlr -f 1 -s 1 -l 100 > ${CUSE_OUT}.8
${NVME_CMD} get-log $ctrlr -i 1 -l 100 > ${CUSE_OUT}.9
${NVME_CMD} reset $ctrlr > ${CUSE_OUT}.10

set -e

for i in {1..10}; do
	if [ -f "${KERNEL_OUT}.${i}" ] && [ -f "${CUSE_OUT}.${i}" ]; then
		sed -i "s/${nvme_name}/nvme0/g" ${KERNEL_OUT}.${i}
		diff --suppress-common-lines ${KERNEL_OUT}.${i} ${CUSE_OUT}.${i}
	fi
done

rm -Rf $testdir/match_files

if [ ! -c "$ctrlr" ]; then
	return 1
fi

trap - SIGINT SIGTERM EXIT
killprocess $spdk_tgt_pid
