#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/iscsi_tgt/common.sh

TRACE_TMP_FOLDER=./tmp-trace
TRACE_PORTER_OUTPUT=${TRACE_TMP_FOLDER}/porter.trace
TRACE_PORTER_NOTICE_LOG=${TRACE_TMP_FOLDER}/porter.notice
TRACE_TOOL_LOG=${TRACE_TMP_FOLDER}/trace.log

delete_tmp_files() {
	rm -rf $TRACE_TMP_FOLDER
}

if [ -z "$TARGET_IP" ]; then
	echo "TARGET_IP not defined in environment"
	exit 1
fi

if [ -z "$INITIATOR_IP" ]; then
	echo "INITIATOR_IP not defined in environment"
	exit 1
fi

timing_enter trace_porter

NUM_TRACE_ENTRIES=4096
MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=4096

rpc_py="$rootdir/scripts/rpc.py"
fio_py="$rootdir/scripts/fio.py"

timing_enter start_iscsi_tgt

echo "start iscsi_tgt with trace enabled"
$ISCSI_APP -m 0xf --num-trace-entries $NUM_TRACE_ENTRIES --tpoint-group-mask 0xf &
iscsi_pid=$!
echo "Process pid: $iscsi_pid"

trap "killprocess $iscsi_pid; exit 1" SIGINT SIGTERM EXIT

waitforlisten $iscsi_pid

echo "iscsi_tgt is listening. Running tests..."

timing_exit start_iscsi_tgt

$rpc_py add_portal_group $PORTAL_TAG $TARGET_IP:$ISCSI_PORT
$rpc_py add_initiator_group $INITIATOR_TAG $INITIATOR_NAME $NETMASK

echo "Create bdevs and target nodes"
CONNECTION_NUMBER=15
for i in $(seq 0 $CONNECTION_NUMBER); do
	malloc_bdev="$($rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE)"
	$rpc_py construct_target_node Target$i Target${i}_alias "${malloc_bdev}:0" $PORTAL_TAG:$INITIATOR_TAG 256 -d
done

sleep 1

iscsiadm -m discovery -t sendtargets -p $TARGET_IP:$ISCSI_PORT
iscsiadm -m node --login -p $TARGET_IP:$ISCSI_PORT

mkdir ${TRACE_TMP_FOLDER}
./app/trace_porter/spdk_trace_porter -s iscsi -i 0 -f ${TRACE_PORTER_OUTPUT} -q 2>${TRACE_PORTER_NOTICE_LOG} &
porter_pid=$!
echo "Trace porter pid: $porter_pid"

trap "iscsicleanup; killprocess $iscsi_pid; killprocess $porter_pid; delete_tmp_files; exit 1" SIGINT SIGTERM EXIT

echo "Running FIO"
$fio_py 131072 32 randrw 1

iscsicleanup
# Delete Malloc blockdevs and targets
for i in $(seq 0 $CONNECTION_NUMBER); do
	$rpc_py delete_target_node iqn.2016-06.io.spdk:Target${i}
	$rpc_py delete_malloc_bdev Malloc${i}
done

trap "delete_tmp_files; exit 1" SIGINT SIGTERM EXIT

killprocess $iscsi_pid
killprocess $porter_pid
./app/trace/spdk_trace -f ${TRACE_PORTER_OUTPUT} > ${TRACE_TOOL_LOG}

#verify trace porter and trace tool
#trace entries str in trace-porter, like "Trace Size of lcore (0): 4136"
porter_num="$(grep "trace entries for lcore" ${TRACE_PORTER_NOTICE_LOG} | cut -d ' ' -f 2)"
#trace entries str in trace-tool, like "Port 4096 trace entries for lcore (0) in 441871 msec"
trace_tool_num="$(grep "Trace Size of lcore" ${TRACE_TOOL_LOG} | cut -d ' ' -f 6)"

delete_tmp_files

echo "entries numbers from trace porter are:" $porter_num
echo "entries numbers from trace tool are:" $trace_tool_num

arr_porter_num=($porter_num)
arr_trace_tool_num=($trace_tool_num)
len_arr_porter_num=${#arr_porter_num[@]}
len_arr_trace_tool_num=${#arr_trace_tool_num[@]}

#lcore num check
if [  $len_arr_porter_num -ne $len_arr_trace_tool_num ]; then
	echo "trace porter test on iscsi: failure on lcore number check"
	set -e
	exit 1
fi
#trace entries num check
for i in $(seq 0 $((len_arr_porter_num - 1))); do
if [  ${arr_porter_num[$i]} -le ${NUM_TRACE_ENTRIES} ]; then
	echo "trace porter test on iscsi: failure on inefficient entries number check"
	set -e
	exit 1
fi
if [  ${arr_porter_num[$i]} -ne ${arr_trace_tool_num[$i]} ]; then
	echo "trace porter test on iscsi: failure on entries number check"
	set -e
	exit 1
fi
done

trap - SIGINT SIGTERM EXIT
timing_exit trace_porter
