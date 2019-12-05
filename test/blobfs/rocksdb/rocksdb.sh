#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh

run_step() {
	if [ -z "$1" ]; then
		echo run_step called with no parameter
		exit 1
	fi

	cat <<- EOL >> "$1"_flags.txt
	--spdk=$ROCKSDB_CONF
	--spdk_bdev=Nvme0n1
	--spdk_cache_size=$CACHE_SIZE
	EOL

	echo -n Start $1 test phase...
	/usr/bin/time taskset 0xFF $DB_BENCH --flagfile="$1"_flags.txt &> "$1"_db_bench.txt
	DB_BENCH_FILE=$(grep /dev/shm "$1"_db_bench.txt | cut -f 6 -d ' ')
	gzip $DB_BENCH_FILE
	mv $DB_BENCH_FILE.gz "$1"_trace.gz
	chmod 644 "$1"_trace.gz
	echo done.
}

run_bsdump() {
	$rootdir/examples/blob/cli/blobcli -c $ROCKSDB_CONF -b Nvme0n1 -D &> bsdump.txt
}

# In the autotest job, we copy the rocksdb source to just outside the spdk directory.
DB_BENCH_DIR="$rootdir/../rocksdb"
DB_BENCH=$DB_BENCH_DIR/db_bench
ROCKSDB_CONF=$testdir/rocksdb.conf

if [ ! -e $DB_BENCH_DIR ]; then
	echo $DB_BENCH_DIR does not exist, skipping rocksdb tests
	exit 0
fi

timing_enter db_bench_build

pushd $DB_BENCH_DIR
if [ -z "$SKIP_GIT_CLEAN" ]; then
	git clean -x -f -d
fi
$MAKE db_bench $MAKEFLAGS $MAKECONFIG DEBUG_LEVEL=0 SPDK_DIR=$rootdir
popd

timing_exit db_bench_build

$rootdir/scripts/gen_nvme.sh > $ROCKSDB_CONF
# 0x80 is the bit mask for BlobFS tracepoints
echo "[Global]" >> $ROCKSDB_CONF
echo "TpointGroupMask 0x80" >> $ROCKSDB_CONF

trap 'run_bsdump; rm -f $ROCKSDB_CONF; exit 1' SIGINT SIGTERM EXIT

if [ -z "$SKIP_MKFS" ]; then
	run_test "blobfs_mkfs" $rootdir/test/blobfs/mkfs/mkfs $ROCKSDB_CONF Nvme0n1
fi

mkdir -p $output_dir/rocksdb
RESULTS_DIR=$output_dir/rocksdb
if [ $RUN_NIGHTLY -eq 1 ]; then
	CACHE_SIZE=4096
	DURATION=60
	NUM_KEYS=100000000
else
	CACHE_SIZE=2048
	DURATION=20
	NUM_KEYS=20000000
fi

cd $RESULTS_DIR
cp $testdir/common_flags.txt insert_flags.txt
cat << EOL >> insert_flags.txt
--benchmarks=fillseq
--threads=1
--disable_wal=1
--use_existing_db=0
--num=$NUM_KEYS
EOL

cp $testdir/common_flags.txt randread_flags.txt
cat << EOL >> randread_flags.txt
--benchmarks=readrandom
--threads=16
--duration=$DURATION
--disable_wal=1
--use_existing_db=1
--num=$NUM_KEYS
EOL

cp $testdir/common_flags.txt overwrite_flags.txt
cat << EOL >> overwrite_flags.txt
--benchmarks=overwrite
--threads=1
--duration=$DURATION
--disable_wal=1
--use_existing_db=1
--num=$NUM_KEYS
EOL

cp $testdir/common_flags.txt readwrite_flags.txt
cat << EOL >> readwrite_flags.txt
--benchmarks=readwhilewriting
--threads=4
--duration=$DURATION
--disable_wal=1
--use_existing_db=1
--num=$NUM_KEYS
EOL

cp $testdir/common_flags.txt writesync_flags.txt
cat << EOL >> writesync_flags.txt
--benchmarks=overwrite
--threads=1
--duration=$DURATION
--disable_wal=0
--use_existing_db=1
--sync=1
--num=$NUM_KEYS
EOL

run_test "rocksdb_insert" run_step insert
run_test "rocksdb_overwrite" run_step overwrite
run_test "rocksdb_readwrite" run_step readwrite
run_test "rocksdb_writesync" run_step writesync
run_test "rocksdb_randread" run_step randread

trap - SIGINT SIGTERM EXIT

run_bsdump
rm -f $ROCKSDB_CONF
