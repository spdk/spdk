#!/usr/bin/env bash

set -e

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/..)

if [ $(uname -s) = Linux ] && [ -f /usr/sbin/sgdisk ]; then
	conf=$1
	devname=$2

	if [ ! -e $conf ]; then
		exit 0
	fi

        modprobe nbd
        $rootdir/test/lib/bdev/nbd/nbd -c $conf -b $devname -n /dev/nbd0 &
        nbd_pid=$!
        echo "Process nbd pid: $nbd_pid"
        waitforlisten $nbd_pid 5260

        # Make sure nbd process still run. The reaon is that:
        # nbd program may exist by calling spdk_app_stop due to some issues in
        # nbd_start function. And we can not catch the abnormal exit.
        sleep 5

        if [ `ps aux | awk -F " " '{print $2}' | grep $nbd_pid` ]; then
                if [ -e /dev/nbd0 ]; then
                        parted -s /dev/nbd0 mklabel gpt mkpart primary '0%' '50%' mkpart primary '50%' '100%'
                        #change the GUID to SPDK GUID value
                        /usr/sbin/sgdisk -u 1:$SPDK_GPT_UUID /dev/nbd0
                        /usr/sbin/sgdisk -u 2:$SPDK_GPT_UUID /dev/nbd0
                fi

                killprocess $nbd_pid
        fi
fi
