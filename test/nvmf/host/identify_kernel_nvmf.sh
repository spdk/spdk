#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/scripts/autotest_common.sh
source $rootdir/test/nvmf/common.sh

set -e

RDMA_IP_LIST=$(get_available_rdma_ips)
NVMF_FIRST_TARGET_IP=$(echo "$RDMA_IP_LIST" | head -n 1)
if [ -z $NVMF_FIRST_TARGET_IP ]; then
	echo "no NIC for nvmf test"
	exit 0
fi

timing_enter identify_kernel_nvmf_tgt

subsystemname=nqn.2016-06.io.spdk:testnqn

modprobe null_blk nr_devices=1
modprobe nvmet
modprobe nvmet-rdma

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

sleep 2

$rootdir/examples/nvme/identify/identify -r "\
	trtype:RDMA \
	adrfam:IPv4 \
	traddr:$NVMF_FIRST_TARGET_IP \
	trsvcid:$NVMF_PORT \
	subnqn:nqn.2014-08.org.nvmexpress.discovery" -t all

rm -rf /sys/kernel/config/nvmet/ports/1/subsystems/$subsystemname

echo 0 > /sys/kernel/config/nvmet/subsystems/$subsystemname/namespaces/1/enable
echo -n 0 > /sys/kernel/config/nvmet/subsystems/$subsystemname/namespaces/1/device_path

rmdir --ignore-fail-on-non-empty /sys/kernel/config/nvmet/subsystems/$subsystemname/namespaces/1
rmdir --ignore-fail-on-non-empty /sys/kernel/config/nvmet/subsystems/$subsystemname
rmdir --ignore-fail-on-non-empty /sys/kernel/config/nvmet/ports/1

rmmod nvmet-rdma
rmmod null_blk
rmmod nvmet

timing_exit identify_kernel_nvmf_tgt
