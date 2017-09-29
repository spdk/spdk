#!/usr/bin/env bash
set -ex
BASE_DIR=$(readlink -f $(dirname $0))
ROOT_DIR=$(readlink -f $BASE_DIR/../../..)
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $BASE_DIR/../../../../ && pwd)"

PMEM_BDEVS=""
PMEM_SIZE=128
PMEM_BLOCK_SIZE=512
SUBSYS_NR=0

function clear_pmem_pool()
{
	for pmem in $PMEM_BDEVS; do
		$rpc_py delete_bdev $pmem
	done

	for i in `seq 0 $SUBSYS_NR`; do
		for c in `seq 0 $SUBSYS_NR`; do
			$rpc_py delete_pmem_pool /tmp/pool_file$i$c
		done
	done
}

if [[ $EUID -ne 0 ]]; then
	echo "Go away user come back as root"
	exit 1
fi

rpc_py="python $ROOT_DIR/scripts/rpc.py"
fio_py="python $BASE_DIR/../fiotest/run_fio.py"

. $BASE_DIR/../common/common.sh

$BASE_DIR/../fiotest/run_vhost.sh $x --work-dir=$BASE_DIR/vhost.conf &
pid=$?
sleep 10
trap "spdk_vhost_kill; rm -f /tmp/pool_file*; exit 1" SIGINT SIGTERM EXIT

for i in `seq 0 $SUBSYS_NR`; do
	for c in `seq 0 $SUBSYS_NR`; do
		$rpc_py create_pmem_pool /tmp/pool_file$i$c $PMEM_SIZE $PMEM_BLOCK_SIZE
		PMEM_BDEVS+="$($rpc_py construct_pmem_bdev /tmp/pool_file$i$c)"
	done
done

run_fio+="$fio_bin "
run_fio+="--job-file="
for job in $fio_jobs; do
	run_fio+="$job,"
done
run_fio="${run_fio::-1}"
run_fio+=" "
run_fio+="--out=$TEST_DIR "

if [[ ! $disk_split == '' ]]; then
	run_fio+="--split-disks=$disk_split "
fi

$fio_py 1048576 64 randwrite 10 verify

rm -f ./local-job0-0-verify.state

trap - SIGINT SIGTERM EXIT

rm -f ./local-job*
clear_pmem_pool
killprocess $pid
#spdk_vhost_kill
timing_exit vhost_pmem
