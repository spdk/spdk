#!/usr/bin/env bash

set -e

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
plugindir=$rootdir/examples/bdev/fio_plugin

function run_fio()
{
	if [ $RUN_NIGHTLY -eq 0 ]; then
		LD_PRELOAD=$plugindir/fio_plugin /usr/src/fio/fio --ioengine=spdk_bdev --iodepth=8 --bs=4k --runtime=10 $testdir/bdev.fio "$@"
	else
		# Use size 192KB which both exceeds typical 128KB max NVMe I/O
		#  size and will cross 128KB Intel DC P3700 stripe boundaries.
		LD_PRELOAD=$plugindir/fio_plugin /usr/src/fio/fio --ioengine=spdk_bdev --iodepth=128 --bs=192k --runtime=100 $testdir/bdev.fio "$@"
	fi
}

source $rootdir/test/vhost/common/common.sh
source $rootdir/scripts/autotest_common.sh

#spdk_vhost_run $testdir/initiator
spdk_vhost_run $testdir/initiator

timing_enter bdev

path=$(echo "$rootdir" | sed 's|/|\\/|g')
# Create a file to be used as an AIO backend
dd if=/dev/zero of=/$rootdir/../vhost/aiofile bs=2048 count=5000

cp $testdir/bdev.conf.in $testdir/bdev.conf
path=$(echo "$rootdir" | sed 's|/|\\/|g')
sed -i "s|/tmp|$path/../vhost|g" $testdir/bdev.conf
#$rootdir/scripts/gen_nvme.sh >> $testdir/bdev.conf

timing_enter bounds
$rootdir/test/lib/bdev/bdevio/bdevio $testdir/bdev.conf
timing_exit bounds

timing_enter nbd
if grep -q Nvme0 $testdir/bdev.conf; then
	part_dev_by_gpt $testdir/bdev.conf Nvme0n1 $rootdir
fi
timing_exit nbd

timing_enter bdev_svc
bdevs=$(discover_bdevs $rootdir $testdir/bdev.conf 5261 | jq -r '.[] | select(.bdev_opened_for_write == false)')
timing_exit bdev_svc

#if [ -d /usr/src/fio ] && [ $SPDK_RUN_ASAN -eq 0 ]; then
if [ -d /usr/src/fio ]; then
	timing_enter fio
        for rw in "write" "read" "randwrite" "randread" "rw" "randrw"; do
	        timing_enter fio_rw_verify
                cp $testdir/../common/fio_jobs/default_initiator.job $testdir/bdev.fio
                if [ $rw == "randread" ]; then
                         echo "size=2m" >> $testdir/bdev.fio
                         echo "io_size=10m" >> $testdir/bdev.fio
                fi
                if [ $rw == "read" ] || [ $rw == "randread" ]; then
                        echo "[job_write]" >> $testdir/bdev.fio
                        echo "stonewall" >> $testdir/bdev.fio
                        echo "rw=write" >> $testdir/bdev.fio
                        echo "do_verify=0" >> $testdir/bdev.fio
                        echo -n "filename=" >> $testdir/bdev.fio
                        for b in $(echo $bdevs | jq -r '.name'); do
                                echo -n "$b:" >> $testdir/bdev.fio
                        done
                        echo "" >> $testdir/bdev.fio
                fi
                echo "[job_$rw]" >> $testdir/bdev.fio
                echo "stonewall" >> $testdir/bdev.fio
                echo "rw=$rw" >> $testdir/bdev.fio
                echo -n "filename=" >> $testdir/bdev.fio
	        for b in $(echo $bdevs | jq -r '.name'); do
	        	echo -n "$b:" >> $testdir/bdev.fio
                done

                cat $testdir/bdev.fio
	        run_fio --spdk_conf=$testdir/bdev.conf

	        rm -f *.state
	        rm -f $testdir/bdev.fio
	        timing_exit fio_rw_verify
        done
	#timing_enter fio_trim
	## Generate the fio config file given the list of all unclaimed bdevs that support unmap
	#fio_config_gen $testdir/bdev.fio trim
	#for b in $(echo $bdevs | jq -r 'select(.supported_io_types.unmap == true) | .name'); do
	#	fio_config_add_job $testdir/bdev.fio $b
	#done

	#run_fio --spdk_conf=$testdir/bdev.conf

	#rm -f *.state
	#rm -f $testdir/bdev.fio
	#timing_exit fio_trim

	timing_exit fio
fi

if [ $RUN_NIGHTLY -eq 1 ]; then
	# Temporarily disabled - infinite loop
	timing_enter reset
	#$testdir/bdevperf/bdevperf -c $testdir/bdev.conf -q 16 -w reset -s 4096 -t 60
	timing_exit reset
fi


#if grep -q Nvme0 $testdir/bdev.conf; then
#	part_dev_by_gpt $testdir/bdev.conf Nvme0n1 $rootdir reset
#fi

rm -f /tmp/aiofile
rm -f $testdir/bdev.conf
spdk_vhost_kill
timing_exit bdev
