#!/usr/bin/env bash
set -xe

basedir=$(readlink -f $(dirname $0))
MAKE="make -j$(( $(nproc)  * 2 ))"

if [[ $1 == "scsi" ]]; then
    devs=""
    for entry in /sys/block/sd*; do
        if grep -Eq '(INTEL|RAWSCSI|LIO-ORG)' $entry/device/vendor; then
            devs+="$(basename $entry)"
        fi
    done
else script=$blk_script;
    devs=$(cd /sys/block; echo vd*)
fi

trap "exit 1" SIGINT SIGTERM EXIT
for dev in $devs; do
        mkfs_cmd="mkfs.$fs"
        parted_cmd="parted -s /dev/${dev}"
        if [ "ntfs" == $fs ]; then
            mkfs_cmd+=" -f -F"
        elif [ "btrfs" == $fs ]; then
            mkfs_cmd+=" -f"
        elif [ "fat" == $fs ]; then
            mkfs_cmd+=" -I"
        else
            mkfs_cmd+=" -F"
        fi

        echo "INFO: Creating partition table on disk using: $parted_cmd mklabel gpt"
        $parted_cmd mklabel gpt
        $parted_cmd mkpart primary 2048s 100%
        sleep 2

        mkfs_cmd+=" /dev/${dev}1"
        echo "INFO: Creating filesystem using: $mkfs_cmd"
        $mkfs_cmd

        mkdir -p /mnt/${dev}dir
        mount -o sync /dev/${dev}1 /mnt/${dev}dir
        mkdir -p /mnt/${dev}dir/linux-src
        tar xf $basedir/linux-src.tar.gz -C /mnt/${dev}dir/linux-src --strip-components=1
        sleep 2

        # Now build SPDK
        $MAKE -C /mnt/${dev}dir/linux-src defconfig
        $MAKE -C /mnt/${dev}dir/linux-src
        # Print out space consumed on target device
        df -h /dev/$dev
        rm -rf /mnt/${dev}dir/linux-src
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
