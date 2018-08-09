#!/usr/bin/env bash
rootdir=$(readlink -f $(dirname $0))/../..
source "$rootdir/test/common/autotest_common.sh"

set -e

DEFAULT_VM_IMAGE="/home/sys_sgsw/vhost_vm_image.qcow2"
CENTOS_VM_IMAGE="/home/sys_sgsw/spdk_vhost_CentOS_vm_image.qcow2"
DEFAULT_FIO_BIN="/home/sys_sgsw/fio_ubuntu"
CENTOS_FIO_BIN="/home/sys_sgsw/fio_ubuntu_bak"

case $1 in
	-h|--help)
		echo "usage: $(basename $0) TEST_TYPE"
		echo "Test type can be:"
		echo "  -i |--integrity                      for running an integrity test with vhost scsi"
		echo "  -fs|--fs-integrity-scsi              for running an integrity test with filesystem"
		echo "  -fb|--fs-integrity-blk               for running an integrity test with filesystem"
		echo "  -p |--performance                    for running a performance test with vhost scsi"
		echo "  -ib|--integrity-blk                  for running an integrity test with vhost blk"
		echo "  -pb|--performance-blk                for running a performance test with vhost blk"
		echo "  -ils|--integrity-lvol-scsi           for running an integrity test with vhost scsi and lvol backends"
		echo "  -ilb|--integrity-lvol-blk            for running an integrity test with vhost blk and lvol backends"
		echo "  -ilsn|--integrity-lvol-scsi-nightly  for running an nightly integrity test with vhost scsi and lvol backends"
		echo "  -ilbn|--integrity-lvol-blk-nightly   for running an nightly integrity test with vhost blk and lvol backends"
		echo "  -hp|--hotplug                        for running hotplug tests"
		echo "  -shr|--scsi-hot-remove               for running scsi hot remove tests"
		echo "  -bhr|--blk-hot-remove                for running blk hot remove tests"
		echo "  -ro|--readonly                       for running readonly test for vhost blk"
		echo "  -b|--boot                            for booting vm from vhost controller"
		echo "  -h |--help                           prints this message"
		echo ""
		echo "Environment:"
		echo "  VM_IMAGE        path to QCOW2 VM image used during test (default: $DEFAULT_VM_IMAGE)"
		echo ""
		echo "Tests are performed only on Linux machine. For other OS no action is performed."
		echo ""
		exit 0;
		;;
esac

echo "Running SPDK vhost fio autotest..."
if [[ $(uname -s) != Linux ]]; then
	echo ""
	echo "INFO: Vhost tests are only for Linux machine."
	echo ""
	exit 0
fi

: ${VM_IMAGE="$DEFAULT_VM_IMAGE"}
: ${FIO_BIN="$DEFAULT_FIO_BIN"}

if [[ ! -r "${VM_IMAGE}" ]]; then
	echo ""
	echo "ERROR: VM image '${VM_IMAGE}' does not exist."
	echo ""
	exit 1
fi

DISKS_NUMBER=`lspci -mm -n | grep 0108 | tr -d '"' | awk -F " " '{print "0000:"$1}'| wc -l`

WORKDIR=$(readlink -f $(dirname $0))

case $1 in
	-n|--negative)
		echo 'Negative tests suite...'
		run_test case $WORKDIR/other/negative.sh
		report_test_completion "vhost_negative"
		;;
	-p|--performance)
		echo 'Running performance suite...'
		run_test case $WORKDIR/fiotest/autotest.sh --fio-bin=$FIO_BIN \
		--vm=0,$VM_IMAGE,Nvme0n1p0 \
		--test-type=spdk_vhost_scsi \
		--fio-job=$WORKDIR/common/fio_jobs/default_performance.job
		report_test_completion "vhost_perf"
		;;
	-pb|--performance-blk)
		echo 'Running blk performance suite...'
		run_test case $WORKDIR/fiotest/autotest.sh --fio-bin=$FIO_BIN \
		--vm=0,$VM_IMAGE,Nvme0n1p0 \
		--test-type=spdk_vhost_blk \
		--fio-job=$WORKDIR/common/fio_jobs/default_performance.job
		report_test_completion "vhost_perf_blk"
		;;
	-m|--migration)
		echo 'Running migration suite...'
		run_test case $WORKDIR/migration/migration.sh -x \
		--fio-bin=$FIO_BIN --os=$VM_IMAGE --test-cases=1,2
		;;
	-i|--integrity)
		echo 'Running SCSI integrity suite...'
		run_test case $WORKDIR/fiotest/autotest.sh -x --fio-bin=$FIO_BIN \
		--vm=0,$VM_IMAGE,Nvme0n1p0:Nvme0n1p1:Nvme0n1p2:Nvme0n1p3 \
		--test-type=spdk_vhost_scsi \
		--fio-job=$WORKDIR/common/fio_jobs/default_integrity.job
		report_test_completion "nightly_vhost_integrity"
		;;
	-ib|--integrity-blk)
		echo 'Running blk integrity suite...'
		run_test case $WORKDIR/fiotest/autotest.sh -x --fio-bin=$FIO_BIN \
		--vm=0,$VM_IMAGE,Nvme0n1p0:Nvme0n1p1:Nvme0n1p2:Nvme0n1p3 \
		--test-type=spdk_vhost_blk \
		--fio-job=$WORKDIR/common/fio_jobs/default_integrity.job
		report_test_completion "nightly_vhost_integrity_blk"
		;;
	-fs|--fs-integrity-scsi)
		echo 'Running filesystem integrity suite with SCSI...'
		run_test case $WORKDIR/integrity/integrity_start.sh --ctrl-type=spdk_vhost_scsi --fs="xfs ntfs btrfs ext4"
		report_test_completion "vhost_fs_integrity_scsi"
		;;
	-fb|--fs-integrity-blk)
		echo 'Running filesystem integrity suite with BLK...'
		run_test case $WORKDIR/integrity/integrity_start.sh --ctrl-type=spdk_vhost_blk --fs="xfs ntfs btrfs ext4"
		report_test_completion "vhost_fs_integrity_blk"
		;;
	-ils|--integrity-lvol-scsi)
		echo 'Running lvol integrity suite...'
		run_test case $WORKDIR/lvol/lvol_test.sh -x --fio-bin=$FIO_BIN \
		--ctrl-type=spdk_vhost_scsi --thin-provisioning
		report_test_completion "vhost_integrity_lvol_scsi"
		;;
	-ilb|--integrity-lvol-blk)
		echo 'Running lvol integrity suite...'
		run_test case $WORKDIR/lvol/lvol_test.sh -x --fio-bin=$FIO_BIN \
		--ctrl-type=spdk_vhost_blk
		report_test_completion "vhost_integrity_lvol_blk"
		;;
	-ilsn|--integrity-lvol-scsi-nightly)
		if [[ $DISKS_NUMBER -ge 2 ]]; then
			echo 'Running lvol integrity nightly suite with two cores and two controllers'
			run_test case $WORKDIR/lvol/lvol_test.sh --fio-bin=$FIO_BIN \
			--ctrl-type=spdk_vhost_scsi --max-disks=2 --distribute-cores --vm-count=2

			echo 'Running lvol integrity nightly suite with one core and two controllers'
			run_test case $WORKDIR/lvol/lvol_test.sh --fio-bin=$FIO_BIN \
			--ctrl-type=spdk_vhost_scsi --max-disks=2 --vm-count=2
		fi
		if [[ -e $CENTOS_VM_IMAGE ]]; then
			echo 'Running lvol integrity nightly suite with different os types'
			run_test case $WORKDIR/lvol/lvol_test.sh --fio-bin=$CENTOS_FIO_BIN \
			--ctrl-type=spdk_vhost_scsi --vm-count=2 --multi-os
		fi
		echo 'Running lvol integrity nightly suite with one core and one controller'
		run_test case $WORKDIR/lvol/lvol_test.sh --fio-bin=$FIO_BIN \
		--ctrl-type=spdk_vhost_scsi --max-disks=1
		;;
	-ilbn|--integrity-lvol-blk-nightly)
		if [[ $DISKS_NUMBER -ge 2 ]]; then
			echo 'Running lvol integrity nightly suite with two cores and two controllers'
			run_test case $WORKDIR/lvol/lvol_test.sh --fio-bin=$FIO_BIN \
			--ctrl-type=spdk_vhost_blk --max-disks=2 --distribute-cores --vm-count=2

			echo 'Running lvol integrity nightly suite with one core and two controllers'
			run_test case $WORKDIR/lvol/lvol_test.sh --fio-bin=$FIO_BIN \
			--ctrl-type=spdk_vhost_blk --max-disks=2 --vm-count=2
		fi
		if [[ -e $CENTOS_VM_IMAGE ]]; then
			echo 'Running lvol integrity nightly suite with different os types'
			run_test case $WORKDIR/lvol/lvol_test.sh --fio-bin=$CENTOS_FIO_BIN \
			--ctrl-type=spdk_vhost_blk --vm-count=2 --multi-os
		fi
		echo 'Running lvol integrity nightly suite with one core and one controller'
		run_test case $WORKDIR/lvol/lvol_test.sh --fio-bin=$FIO_BIN \
		--ctrl-type=spdk_vhost_blk --max-disks=1
		;;
	-hp|--hotplug)
		echo 'Running hotplug tests suite...'
		run_test case $WORKDIR/hotplug/scsi_hotplug.sh --fio-bin=$FIO_BIN \
			--vm=0,$VM_IMAGE,Nvme0n1p0:Nvme0n1p1 \
			--vm=1,$VM_IMAGE,Nvme0n1p2:Nvme0n1p3 \
			--vm=2,$VM_IMAGE,Nvme0n1p4:Nvme0n1p5 \
			--vm=3,$VM_IMAGE,Nvme0n1p6:Nvme0n1p7 \
			--test-type=spdk_vhost_scsi \
			--fio-jobs=$WORKDIR/hotplug/fio_jobs/default_integrity.job -x
		report_test_completion "vhost_hotplug"
		;;
	-shr|--scsi-hot-remove)
		echo 'Running scsi hotremove tests suite...'
		run_test case $WORKDIR/hotplug/scsi_hotplug.sh --fio-bin=$FIO_BIN \
			--vm=0,$VM_IMAGE,Nvme0n1p0:Nvme0n1p1 \
			--vm=1,$VM_IMAGE,Nvme0n1p2:Nvme0n1p3 \
			--test-type=spdk_vhost_scsi \
			--scsi-hotremove-test \
			--fio-jobs=$WORKDIR/hotplug/fio_jobs/default_integrity.job
		;;
	-bhr|--blk-hot-remove)
		echo 'Running blk hotremove tests suite...'
		run_test case $WORKDIR/hotplug/scsi_hotplug.sh --fio-bin=$FIO_BIN \
			--vm=0,$VM_IMAGE,Nvme0n1p0:Nvme0n1p1 \
			--vm=1,$VM_IMAGE,Nvme0n1p2:Nvme0n1p3 \
			--test-type=spdk_vhost_blk \
			--blk-hotremove-test \
			--fio-jobs=$WORKDIR/hotplug/fio_jobs/default_integrity.job
	;;
	-ro|--readonly)
		echo 'Running readonly tests suite...'
		run_test case $WORKDIR/readonly/readonly.sh --vm_image=$VM_IMAGE --disk=Nvme0n1 -x
		report_test_completion "vhost_readonly"
		;;
	-b|--boot)
		echo 'Running os boot from vhost controller...'
		$WORKDIR/vhost_boot/vhost_boot.sh --vm_image=$VM_IMAGE
		report_test_completion "vhost_boot"
		;;
	*)
		echo "unknown test type: $1"
		exit 1
	;;
esac
