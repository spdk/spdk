#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$testdir/../../..
source $rootdir/scripts/autotest_common.sh

script='shopt -s nullglob; \
    for entry in /sys/block/sd*; do \
        disk_type="$(cat $entry/device/vendor)"; \
           if [[ $disk_type == Intel* ]] || [[ $disk_type == RAWSCSI* ]] || [[ $disk_type == LIO-ORG* ]]; then \
                fname=$(basename $entry); \
                echo -n "$fname "; \
           fi; \
    done'

devs="$(echo "$script" | bash -s)"

timing_enter ext4test

trap "exit 1" SIGINT SIGTERM EXIT

for dev in $devs; do
        mkfs.ext4 -F /dev/$dev
        mkdir -p /mnt/${dev}dir
        mount -o sync /dev/$dev /mnt/${dev}dir
        rsync -qav --exclude=".git" $rootdir/ /mnt/${dev}dir/spdk
        sleep 2
        make -C /mnt/${dev}dir/spdk -j8 clean
        make -C /mnt/${dev}dir/spdk -j8

        # Print out space consumed on target device to help decide
        #  if/when we need to increase the size of the malloc LUN
        df -h /dev/$dev
        rm -rf /mnt/${dev}dir/spdk
done

for dev in $devs; do
        umount /mnt/${dev}dir
        rm -rf /mnt/${dev}dir

        stats=( $(cat /sys/block/$dev/stat) )
        echo ""
        echo "$dev stats"
        printf "READ  IO cnt: % 8u merges: % 8u sectors: % 8u ticks: % 8u\n" \
                   ${stats[0]} ${stats[1]} ${stats[2]} ${stats[3]}
        printf "WRITE IO cnt: % 8u merges: % 8u sectors: % 8u ticks: % 8u\n" \
                   ${stats[4]} ${stats[5]} ${stats[6]} ${stats[7]}
        printf "in flight: % 8u io ticks: % 8u time in queue: % 8u\n" \
                   ${stats[8]} ${stats[9]} ${stats[10]}
        echo ""
done

trap - SIGINT SIGTERM EXIT

timing_exit ext4test
