#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/scripts/autotest_common.sh
source $rootdir/test/nvmf/common.sh

LVOL_BDEV_SIZE=24
CONNECTION_NUMBER=12

modprobe -v nvme-rdma
modprobe -v nvme-fabrics

rpc_py="python $rootdir/scripts/rpc.py"

# Disconnect nvmf connection
function disconnect_nvmf()
{
	for i in `seq 1 $CONNECTION_NUMBER`; do
		nvme disconnect -n "nqn.2016-06.io.spdk:cnode$i" || true
	done
}

# Remove lvol bdevs and stores
function remove_backends()
{
	echo "INFO: Removing lvol bdevs"
	for i in `seq 1 $CONNECTION_NUMBER`; do
		lun="lvs$i/lbd_$i"
		$rpc_py delete_bdev $lun
		echo -e "\tINFO: lvol bdev $lun removed"
	done
	sleep 1

	echo "INFO: Removing lvol stores"
	for i in `seq 1 $CONNECTION_NUMBER`; do
		$rpc_py destroy_lvol_store -l lvs$i
		echo -e "\tINFO: lvol store lvs$i removed"
	done
	return 0
}

set -e

# Create conf file for iscsi multiconnection
cat > $testdir/nvmf.conf << EOL
[Nvmf]
  MaxQueuesPerSession 160
  MaxQueueDepth 1024
  MaxIOSize 262144
[Rpc]
  Enable Yes
[Split]
  Split Nvme0n1 40 128
EOL

RDMA_IP_LIST=$(get_available_rdma_ips)
NVMF_FIRST_TARGET_IP=$(echo "$RDMA_IP_LIST" | head -n 1)
if [ -z $NVMF_FIRST_TARGET_IP ]; then
	echo "no NIC for nvmf test"
	exit 0
fi

# SoftRoce does not have enough queues available for
# multiconnection tests. Detect if we're using software RDMA.
# If so - lower the number of subsystems for test.
if check_ip_is_soft_roce $NVMF_FIRST_TARGET_IP; then
	echo "Using software RDMA, lowering number of NVMeOF subsystems."
	CONNECTION_NUMBER=1
fi

# Get Nvme0n1 info through filtering gen_nvme.sh's result
# Only need one nvme and discard the remaining lines from the 13th line
$rootdir/scripts/gen_nvme.sh >> $testdir/nvmf.conf
sed -i '11,$d' $testdir/nvmf.conf

timing_enter multiconnection
timing_enter start_nvmf_tgt
# Start up the NVMf target in another process
$NVMF_APP -c $testdir/nvmf.conf &
pid=$!
trap "disconnect_nvmf; remove_backends; killprocess $pid; exit 1" SIGINT SIGTERM EXIT

waitforlisten $pid ${RPC_PORT}
timing_exit start_nvmf_tgt

# Create nvme backends and creat lvol store on each

# 1 NVMe-OF subsystem per nvme bdev / lvol store / 12 lvol bdevs
# Create lvol bdevs on each lvol store
for i in `seq 1 $CONNECTION_NUMBER`; do
	ls_guid=$($rpc_py construct_lvol_store "Nvme0n1p$i" "lvs$i" -c 1048576)
	$rpc_py construct_lvol_bdev -u $ls_guid lbd_$i $LVOL_BDEV_SIZE
done
sleep 1

for i in `seq 1 $CONNECTION_NUMBER`; do
	lun="lvs$i/lbd_$i:0"
	$rpc_py construct_nvmf_subsystem nqn.2016-06.io.spdk:cnode$i "trtype:RDMA traddr:$NVMF_FIRST_TARGET_IP trsvcid:$NVMF_PORT" '' -a -s SPDK$i -n "$lun"
done
sleep 1

for i in `seq 1 $CONNECTION_NUMBER`; do
	nvme connect -t rdma -n "nqn.2016-06.io.spdk:cnode$i" -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"
done
sleep 1

$testdir/../fio/nvmf_fio.py 131072 64 randrw 5
$testdir/../fio/nvmf_fio.py 262144 16 randwrite 10
sync

disconnect_nvmf

trap - SIGINT SIGTERM EXIT

rm -f $testdir/nvmf.conf
rm -f ./local-job*
nvmfcleanup
kill -9 $pid
timing_exit multiconnection
