#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

nvmftestinit

subsystemname=nqn.2016-06.io.spdk:testnqn

modprobe null_blk nr_devices=1
modprobe nvmet
modprobe nvmet-rdma
modprobe nvmet-fc
modprobe lpfc

if [ ! -d /sys/kernel/config/nvmet/subsystems/$subsystemname ]; then
	mkdir /sys/kernel/config/nvmet/subsystems/$subsystemname
fi
echo 1 > /sys/kernel/config/nvmet/subsystems/$subsystemname/attr_allow_any_host

if [ ! -d /sys/kernel/config/nvmet/subsystems/$subsystemname/namespaces/1 ]; then
	mkdir /sys/kernel/config/nvmet/subsystems/$subsystemname/namespaces/1
fi

echo -n /dev/nullb0 > /sys/kernel/config/nvmet/subsystems/$subsystemname/namespaces/1/device_path
echo 1 > /sys/kernel/config/nvmet/subsystems/$subsystemname/namespaces/1/enable

if [ ! -d /sys/kernel/config/nvmet/ports/1 ]; then
	mkdir /sys/kernel/config/nvmet/ports/1
fi

echo -n rdma > /sys/kernel/config/nvmet/ports/1/addr_trtype
echo -n ipv4 > /sys/kernel/config/nvmet/ports/1/addr_adrfam
echo -n $NVMF_FIRST_TARGET_IP > /sys/kernel/config/nvmet/ports/1/addr_traddr
echo -n $NVMF_PORT > /sys/kernel/config/nvmet/ports/1/addr_trsvcid

ln -s /sys/kernel/config/nvmet/subsystems/$subsystemname /sys/kernel/config/nvmet/ports/1/subsystems/$subsystemname

sleep 4

$SPDK_EXAMPLE_DIR/identify -r "\
	trtype:$TEST_TRANSPORT \
	adrfam:IPv4 \
	traddr:$NVMF_FIRST_TARGET_IP \
	trsvcid:$NVMF_PORT \
	subnqn:nqn.2014-08.org.nvmexpress.discovery" -t all
$SPDK_EXAMPLE_DIR/identify -r "\
	trtype:$TEST_TRANSPORT \
	adrfam:IPv4 \
	traddr:$NVMF_FIRST_TARGET_IP \
	trsvcid:$NVMF_PORT \
	subnqn:$subsystemname"

rm -rf /sys/kernel/config/nvmet/ports/1/subsystems/$subsystemname

echo 0 > /sys/kernel/config/nvmet/subsystems/$subsystemname/namespaces/1/enable
echo -n 0 > /sys/kernel/config/nvmet/subsystems/$subsystemname/namespaces/1/device_path

rmdir --ignore-fail-on-non-empty /sys/kernel/config/nvmet/subsystems/$subsystemname/namespaces/1
rmdir --ignore-fail-on-non-empty /sys/kernel/config/nvmet/subsystems/$subsystemname
rmdir --ignore-fail-on-non-empty /sys/kernel/config/nvmet/ports/1

rmmod lpfc
rmmod nvmet_fc
rmmod nvmet-rdma
rmmod null_blk
rmmod nvmet

nvmftestfini
