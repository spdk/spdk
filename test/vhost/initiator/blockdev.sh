#!/usr/bin/env bash

set -x

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
plugindir=$rootdir/examples/bdev/fio_plugin
rpc_py="$rootdir/scripts/rpc.py"

if [ $RUN_NIGHTLY -eq 1 ]; then
        fio_rw=("write" "randwrite" "rw" "randrw")
else
        fio_rw=("randwrite")
fi

function run_spdk_fio() {
        LD_PRELOAD=$plugindir/fio_plugin /usr/src/fio/fio --ioengine=spdk_bdev --iodepth=128 --bs=4k --runtime=10 $testdir/bdev.fio "$@" --spdk_mem=1024
        fio_status=$?
        if [ $fio_status != 0 ]; then
                spdk_vhost_kill
                exit 1
        fi
        rm -f *.state
        rm -f $testdir/bdev.fio
}

function prepare_fio_job_4G() {
        rw="$1"
        fio_bdevs="$2"
        echo "size=1G" >> $testdir/bdev.fio
        echo "io_size=4G" >> $testdir/bdev.fio
        echo "offset=4G" >> $testdir/bdev.fio
        echo "[job_$rw]" >> $testdir/bdev.fio
        echo "stonewall" >> $testdir/bdev.fio
        echo "rw=$rw" >> $testdir/bdev.fio
        echo -n "filename=" >> $testdir/bdev.fio
        for b in $(echo $fio_bdevs | jq -r '.name'); do
                echo -n "$b:" >> $testdir/bdev.fio
        done
}

function prepare_fio_job_for_unmap() {
        fio_bdevs="$1"
        echo -n "filename=" >> $testdir/bdev.fio
        for b in $(echo $fio_bdevs | jq -r '.name'); do
                echo -n "$b:" >> $testdir/bdev.fio
        done
        echo "" >> $testdir/bdev.fio
        echo "size=100m" >> $testdir/bdev.fio
        echo "io_size=400m" >> $testdir/bdev.fio

        # Check that sequential TRIM/UNMAP operations 'zeroes' disk space
        echo "[trim_sequential]" >> $testdir/bdev.fio
        echo "stonewall" >> $testdir/bdev.fio
        echo "rw=trim" >> $testdir/bdev.fio
        echo "trim_verify_zero=1" >> $testdir/bdev.fio

        # Check that random TRIM/UNMAP operations 'zeroes' disk space
        echo "[trim_random]" >> $testdir/bdev.fio
        echo "stonewall" >> $testdir/bdev.fio
        echo "rw=randtrim" >> $testdir/bdev.fio
        echo "trim_verify_zero=1" >> $testdir/bdev.fio

        # Check that after TRIM/UNMAP operation disk space can be used for read
        # by using write with verify (which implies reads)
        echo "[write]" >> $testdir/bdev.fio
        echo "stonewall" >> $testdir/bdev.fio
        echo "rw=write" >> $testdir/bdev.fio
}

source $rootdir/test/vhost/common/common.sh
$rootdir/scripts/gen_nvme.sh
timing_enter spdk_vhost_run
spdk_vhost_run $testdir
timing_exit spdk_vhost_run
$rpc_py construct_malloc_bdev 128 512
$rpc_py construct_malloc_bdev 128 4096
$rpc_py add_vhost_scsi_lun vhost.0 0 Nvme0n1
$rpc_py add_vhost_scsi_lun vhost.1 0 Malloc0
$rpc_py add_vhost_scsi_lun vhost.2 0 Malloc1
$rpc_py get_bdevs
bdevs=$($rpc_py get_bdevs | jq -r '.[] | .name')

for bdev in $bdevs; do
        timing_enter bdev

        cp $testdir/bdev.conf.in $testdir/bdev.conf
        if [ $bdev == "Nvme0n1" ]; then
                sed -i "s|/tmp/vhost.0|$rootdir/../vhost/vhost.0|g" $testdir/bdev.conf
        elif [ $bdev == "Malloc0" ]; then
                sed -i "s|/tmp/vhost.0|$rootdir/../vhost/vhost.1|g" $testdir/bdev.conf
        else
                sed -i "s|/tmp/vhost.0|$rootdir/../vhost/vhost.2|g" $testdir/bdev.conf
        fi

        timing_enter bounds
        $rootdir/test/lib/bdev/bdevio/bdevio $testdir/bdev.conf
        timing_exit bounds

        timing_enter bdev_svc
        bdevs=$(discover_bdevs $rootdir $testdir/bdev.conf | jq -r '.[] | select(.claimed == false)')
        timing_exit bdev_svc

        if [ -d /usr/src/fio ]; then
                timing_enter fio
                for rw in "${fio_rw[@]}"; do
                        timing_enter fio_rw_verify
                        cp $testdir/../common/fio_jobs/default_initiator.job $testdir/bdev.fio
                        echo "[job_$rw]" >> $testdir/bdev.fio
                        echo "stonewall" >> $testdir/bdev.fio
                        echo "rw=$rw" >> $testdir/bdev.fio
                        echo -n "filename=" >> $testdir/bdev.fio
                        for b in $(echo $bdevs | jq -r '.name'); do
                                echo -n "$b:" >> $testdir/bdev.fio
                        done

                        run_spdk_fio --spdk_conf=$testdir/bdev.conf

                        timing_exit fio_rw_verify
                done

                #Host test for unmap
                timing_enter unmap
                cp $testdir/../common/fio_jobs/default_initiator.job $testdir/bdev.fio
                prepare_fio_job_for_unmap "$bdevs"
                run_spdk_fio --spdk_conf=$testdir/bdev.conf
                timing_exit unmap

                #Host test for +4G
                if [ $bdev == "Nvme0n1" ]; then
                    for rw in "${fio_rw[@]}"; do
                        timing_enter fio_4G_rw_verify
                        notice " Running 4G test $rw for disk $bdev"
                        cp $testdir/../common/fio_jobs/default_initiator.job $testdir/bdev.fio
                        prepare_fio_job_4G "$rw" "$bdevs"
                        run_spdk_fio --spdk_conf=$testdir/bdev.conf
                        timing_exit fio_4G_rw_verify
                    done
                fi
                timing_exit fio
        fi

        rm -f $testdir/bdev.conf
        timing_exit bdev
done
timing_enter spdk_vhost_kill
spdk_vhost_kill
timing_exit spdk_vhost_kill
