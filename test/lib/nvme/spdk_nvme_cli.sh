#!/usr/bin/env bash

set -e

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/scripts/common.sh
source $rootdir/scripts/autotest_common.sh
spdk_nvme_cli="/home/sys_sgsw/nvme-cli"

timing_enter nvme_cli

if [ `uname` = Linux ]; then
        start_stub "-s 2048 -i 0 -m 0xF"
        trap "kill_stub; exit 1" SIGINT SIGTERM EXIT
fi

if [ -d $spdk_nvme_cli ]; then
	bdfs=$(iter_pci_class_code 01 08 02)
	cd $spdk_nvme_cli
	make clean && make
	sed -i 's/spdk=0/spdk=1/g' spdk.conf
	sed -i 's/shm_id=1/shm_id=0/g' spdk.conf
	./nvme help
	./nvme list
	./nvme id-ctrl $bdfs
	./nvme list-ctrl $bdfs
	./nvme get-ns-id $bdfs
	./nvme id-ns $bdfs
	./nvme fw-log $bdfs
	./nvme smart-log $bdfs
	./nvme error-log $bdfs
	./nvme list-ns $bdfs -n 1
	./nvme flush $bdfs -n 1
	./nvme get-feature $bdfs -n 1 -f 1 -s 1 -l 100
	./nvme fw-activate $bdfs -s 1 -a 2
	./nvme get-log $bdfs -n 1 -i 1 -l 100
	echo 'hello world' | ./nvme write $bdfs --data-size=520 --block-count=0
	./nvme read $bdfs -s 0x0 -c 0x0 -z 1024
	./nvme write-uncor $bdfs -n 1 -s 64 -c 0x0
	./nvme reset $bdfs
	./nvme gen-hostnqn
fi

if [ `uname` = Linux ]; then
        trap - SIGINT SIGTERM EXIT
        kill_stub
fi

timing_exit nvme_cli
