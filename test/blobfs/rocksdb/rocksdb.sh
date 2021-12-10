#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh

sanitize_results() {
	process_core
	[[ -d $RESULTS_DIR ]] && chmod 644 "$RESULTS_DIR/"*
}

dump_db_bench_on_err() {
	# Fetch std dump of the last run_step that might have failed
	[[ -e $db_bench ]] || return 0

	# Dump entire *.txt to stderr to clearly see what might have failed
	xtrace_disable
	mapfile -t step_map < "$db_bench"
	printf '%s\n' "${step_map[@]/#/* $step (FAILED)}" >&2
	xtrace_restore
}

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

	db_bench=$1_db_bench.txt
	echo -n Start $1 test phase...
	time taskset 0xFF $DB_BENCH --flagfile="$1"_flags.txt &> "$db_bench"
	DB_BENCH_FILE=$(grep -o '/dev/shm/\(\w\|\.\|\d\|/\)*' "$db_bench")
	gzip $DB_BENCH_FILE
	mv $DB_BENCH_FILE.gz "$1"_trace.gz
	chmod 644 "$1"_trace.gz
	echo done.
}

run_bsdump() {
	# 0x80 is the bit mask for BlobFS tracepoints
	$SPDK_EXAMPLE_DIR/blobcli -j $ROCKSDB_CONF -b Nvme0n1 --tpoint-group-mask 0x80 &> bsdump.txt
}

# In the autotest job, we copy the rocksdb source to just outside the spdk directory.
DB_BENCH_DIR="$rootdir/../rocksdb"
DB_BENCH=$DB_BENCH_DIR/db_bench
ROCKSDB_CONF=$testdir/rocksdb.json

if [ ! -e $DB_BENCH_DIR ]; then
	echo $DB_BENCH_DIR does not exist
	false
fi

timing_enter db_bench_build

pushd $DB_BENCH_DIR
if [ -z "$SKIP_GIT_CLEAN" ]; then
	git clean -x -f -d
fi

EXTRA_CXXFLAGS=""
GCC_VERSION=$(cc -dumpversion | cut -d. -f1)
if ((GCC_VERSION >= 9)) && ((GCC_VERSION < 11)); then
	EXTRA_CXXFLAGS+="-Wno-deprecated-copy -Wno-pessimizing-move -Wno-error=stringop-truncation"
elif ((GCC_VERSION >= 11)); then
	EXTRA_CXXFLAGS+="-Wno-error=range-loop-construct"
fi

$MAKE db_bench $MAKEFLAGS $MAKECONFIG DEBUG_LEVEL=0 SPDK_DIR=../spdk EXTRA_CXXFLAGS="$EXTRA_CXXFLAGS"
popd

timing_exit db_bench_build

$rootdir/scripts/gen_nvme.sh --json-with-subsystems > $ROCKSDB_CONF

trap 'dump_db_bench_on_err; run_bsdump || :; rm -f $ROCKSDB_CONF; sanitize_results; exit 1' SIGINT SIGTERM EXIT

if [ -z "$SKIP_MKFS" ]; then
	# 0x80 is the bit mask for BlobFS tracepoints
	run_test "blobfs_mkfs" $rootdir/test/blobfs/mkfs/mkfs $ROCKSDB_CONF Nvme0n1 --tpoint-group-mask 0x80
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
# Make sure that there's enough memory available for the mempool. Unfortunately,
# db_bench doesn't seem to allocate memory from all numa nodes since all of it
# comes exclusively from node0. With that in mind, try to allocate CACHE_SIZE
# + some_overhead (1G) of pages but only on node0 to make sure that we end up
# with the right amount not allowing setup.sh to split it by using the global
# nr_hugepages setting. Instead of bypassing it completely, we use it to also
# get the right size of hugepages.
HUGEMEM=$((CACHE_SIZE + 1024)) HUGENODE=0 \
	"$rootdir/scripts/setup.sh"

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
sanitize_results
