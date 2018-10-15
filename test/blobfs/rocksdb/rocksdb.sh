#!/usr/bin/env bash

set -ex

run_step() {
	if [ -z "$1" ]; then
		echo run_step called with no parameter
		exit 1
	fi

	echo "--spdk=$ROCKSDB_CONF" >> "$1"_flags.txt
	echo "--spdk_bdev=Nvme0n1" >> "$1"_flags.txt
	echo "--spdk_cache_size=$CACHE_SIZE" >> "$1"_flags.txt

	echo -n Start $1 test phase...
	/usr/bin/time taskset 0xFF $DB_BENCH --flagfile="$1"_flags.txt &> "$1"_db_bench.txt
	echo done.
}

run_bsdump() {
	$rootdir/examples/blob/cli/blobcli -c $ROCKSDB_CONF -b Nvme0n1 -D &> bsdump.txt
}

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh

DB_BENCH_DIR=/usr/src/rocksdb
DB_BENCH=$DB_BENCH_DIR/db_bench
ROCKSDB_CONF=$testdir/rocksdb.conf

if [ ! -e $DB_BENCH_DIR ]; then
	echo $DB_BENCH_DIR does not exist, skipping rocksdb tests
	exit 0
fi

timing_enter rocksdb

timing_enter db_bench_build

pushd $DB_BENCH_DIR
git clean -x -f -d
$MAKE db_bench $MAKEFLAGS $MAKECONFIG DEBUG_LEVEL=0 SPDK_DIR=$rootdir
popd

timing_exit db_bench_build

$rootdir/scripts/gen_nvme.sh > $ROCKSDB_CONF

trap 'run_bsdump; rm -f $ROCKSDB_CONF; exit 1' SIGINT SIGTERM EXIT

timing_enter mkfs
$rootdir/test/blobfs/mkfs/mkfs $ROCKSDB_CONF Nvme0n1
timing_exit mkfs

mkdir $output_dir/rocksdb
RESULTS_DIR=$output_dir/rocksdb
CACHE_SIZE=4096
if [ $RUN_NIGHTLY -eq 1 ]; then
	DURATION=60
	NUM_KEYS=100000000
else
	DURATION=20
	NUM_KEYS=20000000
fi

cd $RESULTS_DIR
cp $testdir/common_flags.txt insert_flags.txt
echo "--benchmarks=fillseq" >> insert_flags.txt
echo "--threads=1" >> insert_flags.txt
echo "--disable_wal=1" >> insert_flags.txt
echo "--use_existing_db=0" >> insert_flags.txt
echo "--num=$NUM_KEYS" >> insert_flags.txt

cp $testdir/common_flags.txt randread_flags.txt
echo "--benchmarks=readrandom" >> randread_flags.txt
echo "--threads=16" >> randread_flags.txt
echo "--duration=$DURATION" >> randread_flags.txt
echo "--disable_wal=1" >> randread_flags.txt
echo "--use_existing_db=1" >> randread_flags.txt
echo "--num=$NUM_KEYS" >> randread_flags.txt

cp $testdir/common_flags.txt overwrite_flags.txt
echo "--benchmarks=overwrite" >> overwrite_flags.txt
echo "--threads=1" >> overwrite_flags.txt
echo "--duration=$DURATION" >> overwrite_flags.txt
echo "--disable_wal=1" >> overwrite_flags.txt
echo "--use_existing_db=1" >> overwrite_flags.txt
echo "--num=$NUM_KEYS" >> overwrite_flags.txt

cp $testdir/common_flags.txt readwrite_flags.txt
echo "--benchmarks=readwhilewriting" >> readwrite_flags.txt
echo "--threads=4" >> readwrite_flags.txt
echo "--duration=$DURATION" >> readwrite_flags.txt
echo "--disable_wal=1" >> readwrite_flags.txt
echo "--use_existing_db=1" >> readwrite_flags.txt
echo "--num=$NUM_KEYS" >> readwrite_flags.txt

cp $testdir/common_flags.txt writesync_flags.txt
echo "--benchmarks=overwrite" >> writesync_flags.txt
echo "--threads=1" >> writesync_flags.txt
echo "--duration=$DURATION" >> writesync_flags.txt
echo "--disable_wal=0" >> writesync_flags.txt
echo "--use_existing_db=1" >> writesync_flags.txt
echo "--sync=1" >> writesync_flags.txt
echo "--num=$NUM_KEYS" >> writesync_flags.txt

timing_enter rocksdb_insert
run_step insert
timing_exit rocksdb_insert

timing_enter rocksdb_overwrite
run_step overwrite
timing_exit rocksdb_overwrite

timing_enter rocksdb_readwrite
run_step readwrite
timing_exit rocksdb_readwrite

timing_enter rocksdb_writesync
run_step writesync
timing_exit rocksdb_writesync

timing_enter rocksdb_randread
run_step randread
timing_exit rocksdb_randread

trap - SIGINT SIGTERM EXIT

run_bsdump
rm -f $ROCKSDB_CONF

report_test_completion "blobfs"
timing_exit rocksdb
