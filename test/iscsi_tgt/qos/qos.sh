#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/iscsi_tgt/common.sh

function check_qos_works_well() {
	local bdev=$1
	local lsize=$3

	local start_rio_count=$($rpc_py get_bdevs_iostat -b $bdev | jq -r '.[1].num_read_ops')
	local start_wio_count=$($rpc_py get_bdevs_iostat -b $bdev | jq -r '.[1].num_write_ops')

	local qos_ope=randwrite
	if [[ "$2"  =~ r_ios || "$2"  =~ r_mbytes ]]; then
		qos_ope=randread
	elif [[ "$2"  =~ rw_ios || "$2"  =~ rw_mbytes ]]; then
		qos_ope=randrw
	fi
	$fio_py 4096 64 $qos_ope 5

	local end_rio_count=$($rpc_py get_bdevs_iostat -b $bdev | jq -r '.[1].num_read_ops')
	local end_wio_count=$($rpc_py get_bdevs_iostat -b $bdev | jq -r '.[1].num_write_ops')

	local delta_io_count=$(($end_rio_count+$end_wio_count-$start_rio_count-$start_wio_count))
	local iops=$(($delta_io_count/5))
	local mbps=$(($iops*4096/1048576))

	local retval qos_type
	if [[ "$2" =~ ios ]]; then
		retval=$(echo "$lsize*0.95 < $iops && $iops < $lsize*1.05" | bc)
		qos_type=iops
	else
		retval=$(echo "$lsize*0.95 < $mbps && $mbps < $lsize*1.05" | bc)
		qos_type=bandwidth
	fi
	if [ $retval -eq 0 ]; then
		echo "Failed to limit the $qos_ope $qos_type of malloc bdev by qos"
		exit 1
	fi
}

function test_qos_case() {
	local bdev=$1
	local type=$2
	local lsize=$3

	$rpc_py set_bdev_qos_limit $bdev "--$type" $lsize
	check_qos_works_well $bdev $type $lsize
	$rpc_py set_bdev_qos_limit $bdev "--$type" 0
}

if [ -z "$TARGET_IP" ]; then
	echo "TARGET_IP not defined in environment"
	exit 1
fi

if [ -z "$INITIATOR_IP" ]; then
	echo "INITIATOR_IP not defined in environment"
	exit 1
fi

timing_enter qos

BDEV=Malloc0
MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=512
rpc_py="$rootdir/scripts/rpc.py"
fio_py="$rootdir/scripts/fio.py"

timing_enter start_iscsi_tgt

$ISCSI_APP &
pid=$!
echo "Process pid: $pid"
trap "killprocess $pid; exit 1" SIGINT SIGTERM EXIT
waitforlisten $pid
echo "iscsi_tgt is listening. Running tests..."

timing_exit start_iscsi_tgt

$rpc_py add_portal_group $PORTAL_TAG $TARGET_IP:$ISCSI_PORT
$rpc_py add_initiator_group $INITIATOR_TAG $INITIATOR_NAME $NETMASK
$rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE
# "Malloc0:0" ==> use Malloc0 blockdev for LUN0
# "1:2" ==> map PortalGroup1 to InitiatorGroup2
# "64" ==> iSCSI queue depth 64
# "-d" ==> disable CHAP authentication
$rpc_py construct_target_node Target1 Target1_alias "$BDEV:0" $PORTAL_TAG:$INITIATOR_TAG 64 -d
sleep 1

iscsiadm -m discovery -t sendtargets -p $TARGET_IP:$ISCSI_PORT
iscsiadm -m node --login -p $TARGET_IP:$ISCSI_PORT

trap "iscsicleanup; killprocess $pid; exit 1" SIGINT SIGTERM EXIT

declare -A QOS_PARAM
QOS_PARAM[rw_ios_per_sec]=20000
QOS_PARAM[r_ios_per_sec]=20000
QOS_PARAM[w_ios_per_sec]=20000
QOS_PARAM[rw_mbytes_per_sec]=50
QOS_PARAM[r_mbytes_per_sec]=50
QOS_PARAM[w_mbytes_per_sec]=50

for key in ${!QOS_PARAM[@]}; do
	test_qos_case $BDEV $key ${QOS_PARAM[$key]}
done
echo "All the iops & bandwidth limiting tests successful"

iscsicleanup
$rpc_py delete_target_node 'iqn.2016-06.io.spdk:Target1'

rm -f ./local-job0-0-verify.state
trap - SIGINT SIGTERM EXIT
killprocess $pid

timing_exit qos
