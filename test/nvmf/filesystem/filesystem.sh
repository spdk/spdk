#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/scripts/autotest_common.sh
source $rootdir/test/nvmf/common.sh

#update the two variables after the env setup
KERNEL_DIR="/home/kernel.tar.gz"
KERNEL_BASE=$(basename $KERNEL_DIR)

rpc_py="python $rootdir/scripts/rpc.py"

function trap_handle() {
	nvme disconnect -n "nqn.2016-06.io.spdk:cnode1" || true
	ps -p $nvmfpid
	retval=$?
	if [ $retval -eq 0 ]; then
		$rpc_py delete_bdev lvs0/lbd_0
		$rpc_py destroy_lvol_store -l lvs0
	fi
}

set -e

RDMA_IP_LIST=$(get_available_rdma_ips)
NVMF_FIRST_TARGET_IP=$(echo "$RDMA_IP_LIST" | head -n 1)
if [ -z $NVMF_FIRST_TARGET_IP ]; then
	echo "no NIC for nvmf test"
	exit 0
fi

timing_enter fs_test
timing_enter start_nvmf_tgt

$rootdir/scripts/gen_nvme.sh >> $testdir/../nvmf.conf
# Start up the NVMf target in another process
$NVMF_APP -c $testdir/../nvmf.conf &
nvmfpid=$!


trap "killprocess $nvmfpid; exit 1" SIGINT SIGTERM EXIT

waitforlisten $nvmfpid
timing_exit start_nvmf_tgt

modprobe -v nvme-rdma

ls_guid="$($rpc_py construct_lvol_store Nvme0n1 lvs0 -c 1048576)"
free_mb=$(get_lvs_free_mb "$ls_guid")
lb_bdevs="$($rpc_py construct_lvol_bdev -u $ls_guid lbd_0 "$free_mb")"
$rpc_py construct_nvmf_subsystem nqn.2016-06.io.spdk:cnode1 "trtype:RDMA traddr:$NVMF_FIRST_TARGET_IP trsvcid:$NVMF_PORT" '' -a -s SPDK001 -n "$lb_bdevs"
nvme connect -t rdma -n "nqn.2016-06.io.spdk:cnode1" -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"

trap "trap_handle; killprocess $nvmfpid; exit 1" SIGINT SIGTERM EXIT

sleep 2
mkdir -p /mnt/device

devs=`lsblk -l -o NAME | grep nvme`

for dev in $devs; do
	timing_enter parted
	parted -s /dev/$dev mklabel msdos  mkpart primary '1%' '100%'
	timing_exit parted
	sleep 1

	for fstype in "ext4" "btrfs" "xfs"; do
		timing_enter $fstype
		if [ $fstype = ext4 ]; then
			force=-F
		else
			force=-f
		fi

		mkfs.${fstype} $force /dev/${dev}p1

		mount /dev/${dev}p1 /mnt/device

        	if [ $RUN_NIGHTLY -eq 1 ]; then
                	cp -r ${KERNEL_DIR} /mnt/device
                	make -C /mnt/device/${KERNEL_BASE} clean
                	make -C /mnt/device/${KERNEL_BASE} -j64
                	rm -fr /mnt/device/${KERNEL_BASE}
                	umount /mountnt/device
		else
			touch /mnt/device/aaa
			sync
			rm /mnt/device/aaa
			sync
			umount /mnt/device
			timing_exit $fstype
		fi
	done

	parted -s /dev/$dev rm 1
done

sync


nvme disconnect -n "nqn.2016-06.io.spdk:cnode1" || true
$rpc_py delete_nvmf_subsystem nqn.2016-06.io.spdk:cnode1
$rpc_py delete_bdev  "lvs0/lbd_0"
$rpc_py destroy_lvol_store -l lvs0

trap - SIGINT SIGTERM EXIT

nvmfcleanup
killprocess $nvmfpid
timing_exit fs_test
