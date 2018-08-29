
#!/usr/bin/env bash
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh
rpc_py="python $rootdir/scripts/rpc.py"
set -e

timing_enter blob_bdev_io_wait

truncate -s 400M $testdir/aio.bdev

$rootdir/test/app/bdev_svc/bdev_svc &
bdev_svc_pid=$!

trap "killprocess $bdev_svc_pid; exit 1" SIGINT SIGTERM EXIT

waitforlisten $bdev_svc_pid
$rpc_py construct_aio_bdev $testdir/aio.bdev aio0 512
lvs=`$rpc_py construct_lvol_store aio0 lvs0`
lvol=`$rpc_py construct_lvol_bdev -l lvs0 lvol0 100`

killprocess $bdev_svc_pid



# Minimal number of bdev io pool (5) and cache (1)
echo "[Bdev]" > $testdir/bdevperf.conf
echo "BdevIoPoolSize 5" >> $testdir/bdevperf.conf
echo "BdevIoCacheSize 1" >> $testdir/bdevperf.conf
echo "[AIO]" >> $testdir/bdevperf.conf
echo "AIO $testdir/aio.bdev aio0 512" >> $testdir/bdevperf.conf

$rootdir/test/bdev/bdevperf/bdevperf -c $testdir/bdevperf.conf -q 128 -o 4096 -w write -t 1
$rootdir/test/bdev/bdevperf/bdevperf -c $testdir/bdevperf.conf -q 128 -o 4096 -w read -t 1
$rootdir/test/bdev/bdevperf/bdevperf -c $testdir/bdevperf.conf -q 128 -o 4096 -w flush -t 1
$rootdir/test/bdev/bdevperf/bdevperf -c $testdir/bdevperf.conf -q 128 -o 4096 -w unmap -t 1

sync
rm -rf $testdir/bdevperf.conf
rm -rf $testdir/aio.bdev
trap - SIGINT SIGTERM EXIT
timing_exit bdev_io_wait
