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

DISKS_NUMBER=$(lspci -mm -n | grep 0108 | tr -d '"' | awk -F " " '{print "0000:"$1}'| wc -l)

WORKDIR=$(readlink -f $(dirname $0))

run_test "vhost_negative" $WORKDIR/other/negative.sh

run_test "vhost_boot" $WORKDIR/vhost_boot/vhost_boot.sh --vm_image=$VM_IMAGE

if [ $RUN_NIGHTLY -eq 1 ]; then
	echo 'Running blk integrity suite...'
	run_test "vhost_blk_integrity" $WORKDIR/fiotest/fio.sh -x --fio-bin=$FIO_BIN \
	--vm=0,$VM_IMAGE,Nvme0n1p0:RaidBdev0:RaidBdev1:RaidBdev2 \
	--test-type=spdk_vhost_blk \
	--fio-job=$WORKDIR/common/fio_jobs/default_integrity.job

	echo 'Running SCSI integrity suite...'
	run_test "vhost_scsi_integrity" $WORKDIR/fiotest/fio.sh -x --fio-bin=$FIO_BIN \
	--vm=0,$VM_IMAGE,Nvme0n1p0:RaidBdev0:RaidBdev1:RaidBdev2 \
	--test-type=spdk_vhost_scsi \
	--fio-job=$WORKDIR/common/fio_jobs/default_integrity.job

	echo 'Running filesystem integrity suite with SCSI...'
	run_test "vhost_scsi_fs_integrity" $WORKDIR/integrity/integrity_start.sh --ctrl-type=spdk_vhost_scsi --fs="xfs ntfs btrfs ext4"

	echo 'Running filesystem integrity suite with BLK...'
	run_test "vhost_blk_fs_integrity" $WORKDIR/integrity/integrity_start.sh --ctrl-type=spdk_vhost_blk --fs="xfs ntfs btrfs ext4"

	if [[ $DISKS_NUMBER -ge 2 ]]; then
		echo 'Running lvol integrity nightly suite with two cores and two controllers'
		run_test "vhost_scsi_2core_2ctrl" $WORKDIR/lvol/lvol_test.sh --fio-bin=$FIO_BIN \
		--ctrl-type=spdk_vhost_scsi --max-disks=2 --distribute-cores --vm-count=2

		echo 'Running lvol integrity nightly suite with one core and two controllers'
		run_test "vhost_scsi_1core_2ctrl" $WORKDIR/lvol/lvol_test.sh --fio-bin=$FIO_BIN \
		--ctrl-type=spdk_vhost_scsi --max-disks=2 --vm-count=2
	fi
	if [[ -e $CENTOS_VM_IMAGE ]]; then
		echo 'Running lvol integrity nightly suite with different os types'
		run_test "vhost_scsi_nightly" $WORKDIR/lvol/lvol_test.sh --fio-bin=$CENTOS_FIO_BIN \
		--ctrl-type=spdk_vhost_scsi --vm-count=2 --multi-os
	fi
	echo 'Running lvol integrity nightly suite with one core and one controller'
	run_test "vhost_scsi_1core_1ctrl" $WORKDIR/lvol/lvol_test.sh --fio-bin=$FIO_BIN \
	--ctrl-type=spdk_vhost_scsi --max-disks=1

	if [[ $DISKS_NUMBER -ge 2 ]]; then
		echo 'Running lvol integrity nightly suite with two cores and two controllers'
		run_test "vhost_blk_2core_2ctrl" $WORKDIR/lvol/lvol_test.sh --fio-bin=$FIO_BIN \
		--ctrl-type=spdk_vhost_blk --max-disks=2 --distribute-cores --vm-count=2

		echo 'Running lvol integrity nightly suite with one core and two controllers'
		run_test "vhost_blk_1core_2ctrl" $WORKDIR/lvol/lvol_test.sh --fio-bin=$FIO_BIN \
		--ctrl-type=spdk_vhost_blk --max-disks=2 --vm-count=2
	fi
	if [[ -e $CENTOS_VM_IMAGE ]]; then
		echo 'Running lvol integrity nightly suite with different os types'
		run_test "vhost_blk_nightly" $WORKDIR/lvol/lvol_test.sh --fio-bin=$CENTOS_FIO_BIN \
		--ctrl-type=spdk_vhost_blk --vm-count=2 --multi-os
	fi
	echo 'Running lvol integrity nightly suite with one core and one controller'
	run_test "vhost_lvol_integrity_1core_1ctrl" $WORKDIR/lvol/lvol_test.sh --fio-bin=$FIO_BIN \
	--ctrl-type=spdk_vhost_blk --max-disks=1

	echo 'Running readonly tests suite...'
	run_test "vhost_readonly" $WORKDIR/readonly/readonly.sh --vm_image=$VM_IMAGE --disk=Nvme0n1 -x

	echo 'Running migration suite...'
	run_test "vhost_migration" $WORKDIR/migration/migration.sh -x \
	--fio-bin=$FIO_BIN --os=$VM_IMAGE
fi

echo 'Running lvol integrity suite...'
run_test "vhost_scsi_lvol_integrity" $WORKDIR/lvol/lvol_test.sh -x --fio-bin=$FIO_BIN \
--ctrl-type=spdk_vhost_scsi --thin-provisioning

echo 'Running lvol integrity suite...'
run_test "vhost_blk_lvol_integrity" $WORKDIR/lvol/lvol_test.sh -x --fio-bin=$FIO_BIN \
--ctrl-type=spdk_vhost_blk

run_test "spdkcli_vhost" ./test/spdkcli/vhost.sh
