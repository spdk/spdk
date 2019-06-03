#!/usr/bin/env bash
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/vhost/common.sh

echo "Running SPDK vhost fio autotest..."
if [[ $(uname -s) != Linux ]]; then
	echo ""
	echo "INFO: Vhost tests are only for Linux machine."
	echo ""
	exit 0
fi

CENTOS_VM_IMAGE="/home/sys_sgsw/spdk_vhost_CentOS_vm_image.qcow2"
DEFAULT_FIO_BIN="/home/sys_sgsw/fio_ubuntu"
CENTOS_FIO_BIN="/home/sys_sgsw/fio_ubuntu_bak"

: ${FIO_BIN="$DEFAULT_FIO_BIN"}

if [[ ! -r "${VM_IMAGE}" ]]; then
	echo ""
	echo "ERROR: VM image '${VM_IMAGE}' does not exist."
	echo ""
	exit 1
fi

DISKS_NUMBER=`lspci -mm -n | grep 0108 | tr -d '"' | awk -F " " '{print "0000:"$1}'| wc -l`

WORKDIR=$(readlink -f $(dirname $0))

timing_enter vhost
timing_enter negative
run_test case $WORKDIR/other/negative.sh
report_test_completion "vhost_negative"
timing_exit negative

timing_enter vhost_boot
run_test suite $WORKDIR/vhost_boot/vhost_boot.sh --vm_image=$VM_IMAGE
report_test_completion "vhost_boot"
timing_exit vhost_boot

if [ $RUN_NIGHTLY -eq 1 ]; then
	timing_enter integrity_blk
	echo 'Running blk integrity suite...'
	run_test case $WORKDIR/fiotest/fio.sh -x --fio-bin=$FIO_BIN \
	--vm=0,$VM_IMAGE,Nvme0n1p0:RaidBdev0:RaidBdev1:RaidBdev2 \
	--test-type=spdk_vhost_blk \
	--fio-job=$WORKDIR/common/fio_jobs/default_integrity.job
	report_test_completion "nightly_vhost_integrity_blk"
	timing_exit integrity_blk

	timing_enter integrity
	echo 'Running SCSI integrity suite...'
	run_test case $WORKDIR/fiotest/fio.sh -x --fio-bin=$FIO_BIN \
	--vm=0,$VM_IMAGE,Nvme0n1p0:RaidBdev0:RaidBdev1:RaidBdev2 \
	--test-type=spdk_vhost_scsi \
	--fio-job=$WORKDIR/common/fio_jobs/default_integrity.job
	report_test_completion "nightly_vhost_integrity"
	timing_exit integrity

	timing_enter fs_integrity_scsi
	echo 'Running filesystem integrity suite with SCSI...'
	run_test case $WORKDIR/integrity/integrity_start.sh --ctrl-type=spdk_vhost_scsi --fs="xfs ntfs btrfs ext4"
	report_test_completion "vhost_fs_integrity_scsi"
	timing_exit fs_integrity_scsi

	timing_enter fs_integrity_blk
	echo 'Running filesystem integrity suite with BLK...'
	run_test case $WORKDIR/integrity/integrity_start.sh --ctrl-type=spdk_vhost_blk --fs="xfs ntfs btrfs ext4"
	report_test_completion "vhost_fs_integrity_blk"
	timing_exit fs_integrity_blk

	timing_enter integrity_lvol_scsi_nightly
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
	timing_exit integrity_lvol_scsi_nightly

	timing_enter integrity_lvol_blk_nightly
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
	timing_exit integrity_lvol_blk_nightly

	timing_enter readonly
	echo 'Running readonly tests suite...'
	run_test case $WORKDIR/readonly/readonly.sh --vm_image=$VM_IMAGE --disk=Nvme0n1 -x
	report_test_completion "vhost_readonly"
	timing_exit readonly
fi

if [ $RUN_NIGHTLY_FAILING -eq 1 ]; then
	timing_enter vhost_migration
	echo 'Running migration suite...'
	run_test case $WORKDIR/migration/migration.sh -x \
	--fio-bin=$FIO_BIN --os=$VM_IMAGE --test-cases=1,2
	timing_exit vhost_migration
fi

timing_enter integrity_lvol_scsi
echo 'Running lvol integrity suite...'
run_test case $WORKDIR/lvol/lvol_test.sh -x --fio-bin=$FIO_BIN \
--ctrl-type=spdk_vhost_scsi --thin-provisioning
report_test_completion "vhost_integrity_lvol_scsi"
timing_exit integrity_lvol_scsi

timing_enter integrity_lvol_blk
echo 'Running lvol integrity suite...'
run_test case $WORKDIR/lvol/lvol_test.sh -x --fio-bin=$FIO_BIN \
--ctrl-type=spdk_vhost_blk
report_test_completion "vhost_integrity_lvol_blk"
timing_exit integrity_lvol_blk

timing_enter spdk_cli
run_test suite ./test/spdkcli/vhost.sh
timing_exit spdk_cli

timing_exit vhost
