#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/vhost/common.sh

CENTOS_VM_IMAGE="/home/sys_sgsw/spdk_vhost_CentOS_vm_image.qcow2"
DEFAULT_FIO_BIN="/home/sys_sgsw/fio_ubuntu"
CENTOS_FIO_BIN="/home/sys_sgsw/fio_ubuntu_bak"

case $1 in
	-h | --help)
		echo "usage: $(basename $0) TEST_TYPE"
		echo "Test type can be:"
		echo "  -p |--performance                    for running a performance test with vhost scsi"
		echo "  -pb|--performance-blk                for running a performance test with vhost blk"
		echo "  -hp|--hotplug                        for running hotplug tests"
		echo "  -shr|--scsi-hot-remove               for running scsi hot remove tests"
		echo "  -bhr|--blk-hot-remove                for running blk hot remove tests"
		echo "  -h |--help                           prints this message"
		echo ""
		echo "Environment:"
		echo "  VM_IMAGE        path to QCOW2 VM image used during test (default: $HOME/spdk_test_image.qcow2)"
		echo ""
		echo "Tests are performed only on Linux machine. For other OS no action is performed."
		echo ""
		exit 0
		;;
esac

echo "Running SPDK vhost fio autotest..."
if [[ $(uname -s) != Linux ]]; then
	echo ""
	echo "INFO: Vhost tests are only for Linux machine."
	echo ""
	exit 0
fi

: ${FIO_BIN="$DEFAULT_FIO_BIN"}

if [[ ! -r "${VM_IMAGE}" ]]; then
	echo ""
	echo "ERROR: VM image '${VM_IMAGE}' does not exist."
	echo ""
	exit 1
fi

DISKS_NUMBER=$(lspci -mm -n | grep 0108 | tr -d '"' | awk -F " " '{print "0000:"$1}' | wc -l)

WORKDIR=$(readlink -f $(dirname $0))

case $1 in
	-hp | --hotplug)
		echo 'Running hotplug tests suite...'
		run_test "vhost_hotplug" $WORKDIR/hotplug/scsi_hotplug.sh --fio-bin=$FIO_BIN \
			--vm=0,$VM_IMAGE,Nvme0n1p0:Nvme0n1p1 \
			--vm=1,$VM_IMAGE,Nvme0n1p2:Nvme0n1p3 \
			--vm=2,$VM_IMAGE,Nvme0n1p4:Nvme0n1p5 \
			--vm=3,$VM_IMAGE,Nvme0n1p6:Nvme0n1p7 \
			--test-type=spdk_vhost_scsi \
			--fio-jobs=$WORKDIR/hotplug/fio_jobs/default_integrity.job -x
		;;
	-shr | --scsi-hot-remove)
		echo 'Running scsi hotremove tests suite...'
		run_test "vhost_scsi_hot_remove" $WORKDIR/hotplug/scsi_hotplug.sh --fio-bin=$FIO_BIN \
			--vm=0,$VM_IMAGE,Nvme0n1p0:Nvme0n1p1 \
			--vm=1,$VM_IMAGE,Nvme0n1p2:Nvme0n1p3 \
			--test-type=spdk_vhost_scsi \
			--scsi-hotremove-test \
			--fio-jobs=$WORKDIR/hotplug/fio_jobs/default_integrity.job
		;;
	-bhr | --blk-hot-remove)
		echo 'Running blk hotremove tests suite...'
		run_test "vhost_blk_hot_remove" $WORKDIR/hotplug/scsi_hotplug.sh --fio-bin=$FIO_BIN \
			--vm=0,$VM_IMAGE,Nvme0n1p0:Nvme0n1p1 \
			--vm=1,$VM_IMAGE,Nvme0n1p2:Nvme0n1p3 \
			--test-type=spdk_vhost_blk \
			--blk-hotremove-test \
			--fio-jobs=$WORKDIR/hotplug/fio_jobs/default_integrity.job
		;;
	*)
		echo "unknown test type: $1"
		exit 1
		;;
esac
