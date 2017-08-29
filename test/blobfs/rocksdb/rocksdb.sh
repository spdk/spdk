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
	/usr/bin/time taskset 0xFFF $DB_BENCH --flagfile="$1"_flags.txt &> "$1"_db_bench.txt
	echo done.
}

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/scripts/autotest_common.sh

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

cp $rootdir/etc/spdk/rocksdb.conf.in $ROCKSDB_CONF
$rootdir/scripts/gen_nvme.sh >> $ROCKSDB_CONF

trap 'rm -f $ROCKSDB_CONF; exit 1' SIGINT SIGTERM EXIT

timing_enter mkfs
$rootdir/test/lib/blobfs/mkfs/mkfs $ROCKSDB_CONF Nvme0n1
timing_exit mkfs

mkdir $output_dir/rocksdb
RESULTS_DIR=$output_dir/rocksdb
DURATION=30
NUM_KEYS=50000000
CACHE_SIZE=4096

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

$rootdir/test/lib/blobfs/mkfs/mkfs $ROCKSDB_CONF Nvme0n1

$rootdir/test/lib/blobfs/blobfs_power_failure_test/blobfs_power_failure_test $ROCKSDB_CONF Nvme0n1 power_failure_simulation
$rootdir/test/lib/blobfs/blobfs_power_failure_test/blobfs_power_failure_test $ROCKSDB_CONF Nvme0n1 power_recover_check
if [ $? -eq 0 ]; then
	echo "power failure test success"
else
	echo "power failure test fail"
	exit 1
fi

trap - SIGINT SIGTERM EXIT

rm -f $ROCKSDB_CONF

timing_exit rocksdb
