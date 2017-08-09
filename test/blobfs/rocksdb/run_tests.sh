#!/bin/bash
set -e

if [ $# -eq 0 ]
then
	echo "usage: $0 <location of db_bench>"
	exit 1
fi

DB_BENCH=$(readlink -f $1)
[ -e $DB_BENCH ] || (echo "$DB_BENCH does not exist - needs to be built" && exit 1)

hash mkfs.xfs
: ${USE_PERF:=1}
if ! hash perf; then
	USE_PERF=0
fi
hash python
[ -e /usr/include/gflags/gflags.h ] || (echo "gflags not installed." && exit 1)

# Increase max number of file descriptors.  This will be inherited
#  by processes spawned from this script.
ulimit -n 16384

TESTDIR=$(readlink -f $(dirname $0))

if ls $TESTDIR/results/testrun_* &> /dev/null; then
	mkdir -p $TESTDIR/results/old
	mv $TESTDIR/results/testrun_* $TESTDIR/results/old
fi

if [ -z "$RESULTS_DIR" ]; then
	RESULTS_DIR=$TESTDIR/results/testrun_`date +%Y%m%d_%H%M%S`
	mkdir -p $RESULTS_DIR
	rm -f $TESTDIR/results/last
	ln -s $RESULTS_DIR $TESTDIR/results/last
fi

: ${CACHE_SIZE:=4096}
: ${DURATION:=120}
: ${NUM_KEYS:=500000000}
: ${ROCKSDB_CONF:=/usr/local/etc/spdk/rocksdb.conf}

if [ "$NO_SPDK" = "1" ]
then
	[ -e /dev/nvme0n1 ] || (echo "No /dev/nvme0n1 device node found." && exit 1)
else
	[ -e /dev/nvme0n1 ] && (echo "/dev/nvme0n1 device found - need to run SPDK setup.sh script to bind to UIO." && exit 1)
fi

cd $RESULTS_DIR

SYSINFO_FILE=sysinfo.txt
COMMAND="hostname"
echo ">> $COMMAND : " >> $SYSINFO_FILE
$COMMAND >> $SYSINFO_FILE
echo >> $SYSINFO_FILE

COMMAND="cat /proc/cpuinfo"
echo ">> $COMMAND : " >> $SYSINFO_FILE
$COMMAND >> $SYSINFO_FILE
echo >> $SYSINFO_FILE

COMMAND="cat /proc/meminfo"
echo ">> $COMMAND : " >> $SYSINFO_FILE
$COMMAND >> $SYSINFO_FILE
echo >> $SYSINFO_FILE

if [ "$NO_SPDK" = "1" ]
then
	echo -n Creating and mounting XFS filesystem...
	sudo mkdir -p /mnt/rocksdb
	sudo umount /mnt/rocksdb || true &> /dev/null
	sudo mkfs.xfs -d agcount=32 -l su=4096 -f /dev/nvme0n1 &> mkfs_xfs.txt
	sudo mount -o discard /dev/nvme0n1 /mnt/rocksdb
	sudo chown $USER /mnt/rocksdb
	echo done.
fi

cp $TESTDIR/common_flags.txt insert_flags.txt
echo "--benchmarks=fillseq" >> insert_flags.txt
echo "--threads=1" >> insert_flags.txt
echo "--disable_wal=1" >> insert_flags.txt
echo "--use_existing_db=0" >> insert_flags.txt
echo "--num=$NUM_KEYS" >> insert_flags.txt

cp $TESTDIR/common_flags.txt randread_flags.txt
echo "--benchmarks=readrandom" >> randread_flags.txt
echo "--threads=16" >> randread_flags.txt
echo "--duration=$DURATION" >> randread_flags.txt
echo "--disable_wal=1" >> randread_flags.txt
echo "--use_existing_db=1" >> randread_flags.txt
echo "--num=$NUM_KEYS" >> randread_flags.txt

cp $TESTDIR/common_flags.txt overwrite_flags.txt
echo "--benchmarks=overwrite" >> overwrite_flags.txt
echo "--threads=1" >> overwrite_flags.txt
echo "--duration=$DURATION" >> overwrite_flags.txt
echo "--disable_wal=1" >> overwrite_flags.txt
echo "--use_existing_db=1" >> overwrite_flags.txt
echo "--num=$NUM_KEYS" >> overwrite_flags.txt

cp $TESTDIR/common_flags.txt readwrite_flags.txt
echo "--benchmarks=readwhilewriting" >> readwrite_flags.txt
echo "--threads=4" >> readwrite_flags.txt
echo "--duration=$DURATION" >> readwrite_flags.txt
echo "--disable_wal=1" >> readwrite_flags.txt
echo "--use_existing_db=1" >> readwrite_flags.txt
echo "--num=$NUM_KEYS" >> readwrite_flags.txt

cp $TESTDIR/common_flags.txt writesync_flags.txt
echo "--benchmarks=overwrite" >> writesync_flags.txt
echo "--threads=1" >> writesync_flags.txt
echo "--duration=$DURATION" >> writesync_flags.txt
echo "--disable_wal=0" >> writesync_flags.txt
echo "--use_existing_db=1" >> writesync_flags.txt
echo "--sync=1" >> writesync_flags.txt
echo "--num=$NUM_KEYS" >> writesync_flags.txt

run_step() {
	if [ -z "$1" ]
	then
		echo run_step called with no parameter
		exit 1
	fi

	if [ -z "$NO_SPDK" ]
	then
	  echo "--spdk=$ROCKSDB_CONF" >> "$1"_flags.txt
	  echo "--spdk_bdev=Nvme0n1" >> "$1"_flags.txt
	  echo "--spdk_cache_size=$CACHE_SIZE" >> "$1"_flags.txt
	fi

	if [ "$NO_SPDK" = "1" ]
	then
	  echo "--bytes_per_sync=262144" >> "$1"_flags.txt
	  cat /sys/block/nvme0n1/stat > "$1"_blockdev_stats.txt
	fi

	echo -n Start $1 test phase...
	if [ "$USE_PERF" = "1" ]
	then
		sudo /usr/bin/time taskset 0xFFF perf record $DB_BENCH --flagfile="$1"_flags.txt &> "$1"_db_bench.txt
	else
		sudo /usr/bin/time taskset 0xFFF $DB_BENCH --flagfile="$1"_flags.txt &> "$1"_db_bench.txt
	fi
	echo done.

	if [ "$NO_SPDK" = "1" ]
	then
	  drop_caches
	  cat /sys/block/nvme0n1/stat >> "$1"_blockdev_stats.txt
	fi

	if [ "$USE_PERF" = "1" ]
	then
		echo -n Generating perf report for $1 test phase...
		sudo perf report -f -n | sed '/#/d' | sed '/%/!d' | sort -r > $1.perf.txt
		sudo rm perf.data
		$TESTDIR/postprocess.py `pwd` $1 > $1_summary.txt
	echo done.
	fi
}

drop_caches() {
	echo -n Cleaning Page Cache...
	echo 3 > /proc/sys/vm/drop_caches
	echo done.
}

if [ -z "$SKIP_INSERT" ]
then
	run_step insert
fi
if [ -z "$SKIP_OVERWRITE" ]
then
	run_step overwrite
fi
if [ -z "$SKIP_READWRITE" ]
then
	run_step readwrite
fi
if [ -z "$SKIP_WRITESYNC" ]
then
	run_step writesync
fi
if [ -z "$SKIP_RANDREAD" ]
then
	run_step randread
fi

if [ "$NO_SPDK" = "1" ]
then
	echo -n Unmounting XFS filesystem...
	sudo umount /mnt/rocksdb || true &> /dev/null
	echo done.
fi
