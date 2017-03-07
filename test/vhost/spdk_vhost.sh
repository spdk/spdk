#!/usr/bin/env bash

set -e

if [ ! -f "/home/sys_sgsw/vhost_vm_image.qcow2" ]; then
	echo "VM does not exist, exiting vhost tests without running"
	exit 0
fi

WORKDIR=$(dirname $0)
cd $WORKDIR

param="$1"

if [ $(uname -s) = Linux ]; then

echo Running SPDK vhost fio autotest...

case $param in
    -p|--performance)
	echo Running performance suite...
	./fiotest/autotest.sh --fio-bin=/home/sys_sgsw/fio_ubuntu \
	--vm=0,/home/sys_sgsw/vhost_vm_image.qcow2,Nvme0n1p0 \
	--test-type=spdk_vhost \
	--fio-jobs=$WORKDIR/fiotest/fio_jobs/default_performance.job \
	--qemu-src=/home/sys_sgsw/vhost/qemu
    ;;
    -i|--integrity)
	echo Running integrity suite...
	./fiotest/autotest.sh --fio-bin=/home/sys_sgsw/fio_ubuntu \
	--vm=0,/home/sys_sgsw/vhost_vm_image.qcow2,Nvme0n1p0:Nvme0n1p1:Nvme0n1p2:Nvme0n1p3 \
	--test-type=spdk_vhost \
	--fio-jobs=$WORKDIR/fiotest/fio_jobs/default_integrity.job \
	--qemu-src=/home/sys_sgsw/vhost/qemu
    ;;
	-f|--fs-integrity)
	echo Running filesystem integrity suite...
	VM_IMG=/home/sys_sgsw/vhost_scsi_vm_image.qcow2 ./integrity/integrity_start.sh
	;;
    -h|--help)
	echo "-i|--integrity 		for running an integrity test"
	echo "-f|--fs-integrity 	for running an integrity test with filesystem"
	echo "-p|--performance 		for running a performance test"
	echo "-h|--help 		prints this message"
    ;;
    *)
	echo "unknown test type"
    ;;
esac

fi
