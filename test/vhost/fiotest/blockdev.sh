#!/usr/bin/env bash

set -x

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
plugindir=$rootdir/examples/bdev/fio_plugin

function format_disk_512() {
        $rootdir/scripts/setup.sh reset
        sleep 4
        last_nvme_disk=$( sudo nvme list | tail -n 1 )
        last_nvme_disk="$( cut -d ' ' -f 1 <<< "$last_nvme_disk" )"
        sudo nvme format -l 0 $last_nvme_disk
        sudo NRHUGE=8 $rootdir/scripts/setup.sh
        sleep 4
}

function format_disk_4096() {
        sudo $rootdir/scripts/setup.sh reset
        sleep 4
        last_nvme_disk=$( sudo nvme list | tail -n 1 )
        last_nvme_disk="$( cut -d ' ' -f 1 <<< "$last_nvme_disk" )"
        sudo nvme format -l 3 $last_nvme_disk
        sudo NRHUGE=8 $rootdir/scripts/setup.sh
        sleep 4
}

function run_fio()
{
        LD_PRELOAD=$plugindir/fio_plugin /usr/src/fio/fio --ioengine=spdk_bdev --iodepth=128 --bs=192k --runtime=10 $testdir/bdev.fio "$@"
        fio_status=$?
        if [ $fio_status != 0 ]; then
                spdk_vhost_kill
                exit 1
        fi
}

source $rootdir/test/vhost/common/common.sh
set +e

for block_size in 512 4096; do
        if [ $block_size == 512 ]; then
                cp initiator/vhost.conf.malloc512 initiator/vhost.conf.in
                #format_disk_512
        else
                cp initiator/vhost.conf.malloc4096 initiator/vhost.conf.in
                #format_disk_4096
        fi
        spdk_vhost_run $testdir/initiator
        for bdev_type in "nvme" "malloc"; do
                timing_enter bdev

                cp $testdir/bdev.conf.in $testdir/bdev.conf
                if [ $bdev_type == "malloc" ]; then
                        sed -i "s|/tmp/vhost.0|$rootdir/../vhost/vhost.1|g" $testdir/bdev.conf
                elif [ $bdev_type == "nvme" ]; then
                        sed -i "s|/tmp/vhost.0|$rootdir/../vhost/vhost.0|g" $testdir/bdev.conf
                fi

                timing_enter bounds
                $rootdir/test/lib/bdev/bdevio/bdevio $testdir/bdev.conf
                timing_exit bounds

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

	        timing_enter reset
	        $testdir/bdevperf/bdevperf -c $testdir/bdev.conf -q 16 -w reset -s 4096 -t 60
	        timing_exit reset

                rm -f $testdir/bdev.conf
                timing_exit_bdev
        done
        spdk_vhost_kill
done
