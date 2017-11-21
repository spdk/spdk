#!/usr/bin/env bash

set -e

DEFAULT_VM_IMAGE="/home/sys_sgsw/vhost_vm_image.qcow2"

case $1 in
	-h|--help)
		echo "usage: $(basename $0) TEST_TYPE"
		echo "Test type can be:"
		echo "  -i |--integrity             for running an integrity test with vhost scsi"
		echo "  -fs|--fs-integrity-scsi     for running an integrity test with filesystem"
		echo "  -fb|--fs-integrity-blk      for running an integrity test with filesystem"
		echo "  -p |--performance           for running a performance test with vhost scsi"
		echo "  -ib|--integrity-blk         for running an integrity test with vhost blk"
		echo "  -pb|--performance-blk       for running a performance test with vhost blk"
		echo "  -ils|--integrity-lvol-scsi  for running an integrity test with vhost scsi and lvol backends"
		echo "  -ilb|--integrity-lvol-blk   for running an integrity test with vhost blk and lvol backends"
		echo "  -hp|--hotplug               for running hotplug tests"
		echo "  -h |--help                  prints this message"
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

if [[ ! -r "${VM_IMAGE}" ]]; then
	echo ""
	echo "ERROR: VM image '${VM_IMAGE}' does not exist."
	echo ""
	exit 1
fi

WORKDIR=$(dirname $0)
cd $WORKDIR

case $1 in
	-p|--performance)
		echo 'Running performance suite...'
		./fiotest/autotest.sh --fio-bin=/home/sys_sgsw/fio_ubuntu \
		--vm=0,$VM_IMAGE,Nvme0n1p0 \
		--test-type=spdk_vhost_scsi \
		--fio-jobs=$WORKDIR/common/fio_jobs/default_performance.job \
		--qemu-src=/home/sys_sgsw/vhost/qemu
	;;
	-pb|--performance-blk)
		echo 'Running blk performance suite...'
		./fiotest/autotest.sh --fio-bin=/home/sys_sgsw/fio_ubuntu \
		--vm=0,$VM_IMAGE,Nvme0n1p0 \
		--test-type=spdk_vhost_blk \
		--fio-jobs=$WORKDIR/common/fio_jobs/default_performance.job \
		--qemu-src=/home/sys_sgsw/vhost/qemu
		;;
	-i|--integrity)
		echo 'Running SCSI integrity suite...'
		./fiotest/autotest.sh --fio-bin=/home/sys_sgsw/fio_ubuntu \
		--vm=0,$VM_IMAGE,Nvme0n1p0:Nvme0n1p1:Nvme0n1p2:Nvme0n1p3 \
		--test-type=spdk_vhost_scsi \
		--fio-jobs=$WORKDIR/common/fio_jobs/default_integrity.job \
		--qemu-src=/home/sys_sgsw/vhost/qemu -x
		;;
	-ib|--integrity-blk)
		echo 'Running blk integrity suite...'
		./fiotest/autotest.sh --fio-bin=/home/sys_sgsw/fio_ubuntu \
		--vm=0,$VM_IMAGE,Nvme0n1p0:Nvme0n1p1:Nvme0n1p2:Nvme0n1p3 \
		--test-type=spdk_vhost_blk \
		--fio-jobs=$WORKDIR/common/fio_jobs/default_integrity.job \
		--qemu-src=/home/sys_sgsw/vhost/qemu -x
		;;
	-fs|--fs-integrity-scsi)
		echo 'Running filesystem integrity suite...'
		./integrity/integrity_start.sh -i $VM_IMAGE -m scsi -f ntfs
		;;
	-fb|--fs-integrity-blk)
		echo 'Running filesystem integrity suite...'
		./integrity/integrity_start.sh -i $VM_IMAGE -m blk -f ntfs
		;;
	-ils|--integrity-lvol-scsi)
		echo 'Running lvol integrity suite...'
		./lvol/lvol_test.sh --fio-bin=/home/sys_sgsw/fio_ubuntu \
		--ctrl-type=vhost_scsi
		;;
	-ilb|--integrity-lvol-blk)
		echo 'Running lvol integrity suite...'
		./lvol/lvol_test.sh --fio-bin=/home/sys_sgsw/fio_ubuntu \
		--ctrl-type=vhost_blk
		;;
	-hp|--hotplug)
		echo 'Running hotplug tests suite...'
		./hotplug/scsi_hotplug.sh --fio-bin=/home/sys_sgsw/fio_ubuntu \
			--vm=0,$VM_IMAGE,Nvme0n1p0:Nvme0n1p1 \
			--vm=1,$VM_IMAGE,Nvme0n1p2:Nvme0n1p3 \
			--vm=2,$VM_IMAGE,Nvme0n1p4:Nvme0n1p5 \
			--vm=3,$VM_IMAGE,Nvme0n1p6:Nvme0n1p7 \
			--test-type=spdk_vhost_scsi \
			--fio-jobs=$WORKDIR/hotplug/fio_jobs/default_integrity.job -x
		;;
	*)
		echo "unknown test type: $1"
		exit 1
	;;
esac
