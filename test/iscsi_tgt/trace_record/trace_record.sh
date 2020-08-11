#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/iscsi_tgt/common.sh

iscsitestinit

TRACE_TMP_FOLDER=./tmp-trace
TRACE_RECORD_OUTPUT=${TRACE_TMP_FOLDER}/record.trace
TRACE_RECORD_NOTICE_LOG=${TRACE_TMP_FOLDER}/record.notice
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

NUM_TRACE_ENTRIES=4096
MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=4096

rpc_py="$rootdir/scripts/rpc.py"
fio_py="$rootdir/scripts/fio.py"

timing_enter start_iscsi_tgt

echo "start iscsi_tgt with trace enabled"
"${ISCSI_APP[@]}" -m 0xf --num-trace-entries $NUM_TRACE_ENTRIES --tpoint-group-mask 0xf &
iscsi_pid=$!
echo "Process pid: $iscsi_pid"

trap 'killprocess $iscsi_pid; iscsitestfini; exit 1' SIGINT SIGTERM EXIT

waitforlisten $iscsi_pid

echo "iscsi_tgt is listening. Running tests..."

timing_exit start_iscsi_tgt

mkdir -p ${TRACE_TMP_FOLDER}
./build/bin/spdk_trace_record -s iscsi -p ${iscsi_pid} -f ${TRACE_RECORD_OUTPUT} -q 1> ${TRACE_RECORD_NOTICE_LOG} &
record_pid=$!
echo "Trace record pid: $record_pid"

RPCS=
RPCS+="iscsi_create_portal_group $PORTAL_TAG $TARGET_IP:$ISCSI_PORT\n"
RPCS+="iscsi_create_initiator_group $INITIATOR_TAG $INITIATOR_NAME $NETMASK\n"

echo "Create bdevs and target nodes"
CONNECTION_NUMBER=15
for i in $(seq 0 $CONNECTION_NUMBER); do
	RPCS+="bdev_malloc_create $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE -b Malloc$i\n"
	RPCS+="iscsi_create_target_node Target$i Target${i}_alias "Malloc$i:0" $PORTAL_TAG:$INITIATOR_TAG 256 -d\n"
done
echo -e $RPCS | $rpc_py

sleep 1

iscsiadm -m discovery -t sendtargets -p $TARGET_IP:$ISCSI_PORT
iscsiadm -m node --login -p $TARGET_IP:$ISCSI_PORT
waitforiscsidevices $((CONNECTION_NUMBER + 1))

trap 'iscsicleanup; killprocess $iscsi_pid; killprocess $record_pid; delete_tmp_files; iscsitestfini; exit 1' SIGINT SIGTERM EXIT

echo "Running FIO"
$fio_py -p iscsi -i 131072 -d 32 -t randrw -r 1

iscsicleanup

RPCS=
# Delete Malloc blockdevs and targets
for i in $(seq 0 $CONNECTION_NUMBER); do
	RPCS+="iscsi_delete_target_node iqn.2016-06.io.spdk:Target$i\n"
	RPCS+="bdev_malloc_delete Malloc$i\n"
done
echo -e $RPCS | $rpc_py

trap 'delete_tmp_files; iscsitestfini; exit 1' SIGINT SIGTERM EXIT

killprocess $iscsi_pid
killprocess $record_pid
./build/bin/spdk_trace -f ${TRACE_RECORD_OUTPUT} > ${TRACE_TOOL_LOG}

#verify trace record and trace tool
#trace entries str in trace-record, like "Trace Size of lcore (0): 4136"
record_num="$(grep "trace entries for lcore" ${TRACE_RECORD_NOTICE_LOG} | cut -d ' ' -f 2)"

#trace entries str in trace-tool, like "Port 4096 trace entries for lcore (0) in 441871 msec"
trace_tool_num="$(grep "Trace Size of lcore" ${TRACE_TOOL_LOG} | cut -d ' ' -f 6)"

delete_tmp_files

echo "entries numbers from trace record are:" $record_num
echo "entries numbers from trace tool are:" $trace_tool_num

arr_record_num=($record_num)
arr_trace_tool_num=($trace_tool_num)
len_arr_record_num=${#arr_record_num[@]}
len_arr_trace_tool_num=${#arr_trace_tool_num[@]}

#lcore num check
if [ $len_arr_record_num -ne $len_arr_trace_tool_num ]; then
	echo "trace record test on iscsi: failure on lcore number check"
	set -e
	exit 1
fi
#trace entries num check
for i in $(seq 0 $((len_arr_record_num - 1))); do
	if [ ${arr_record_num[$i]} -le ${NUM_TRACE_ENTRIES} ]; then
		echo "trace record test on iscsi: failure on inefficient entries number check"
		set -e
		exit 1
	fi
	if [ ${arr_record_num[$i]} -ne ${arr_trace_tool_num[$i]} ]; then
		echo "trace record test on iscsi: failure on entries number check"
		set -e
		exit 1
	fi
done

trap - SIGINT SIGTERM EXIT
iscsitestfini
