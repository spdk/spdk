#!/usr/bin/env bash

set -e

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)

source $rootdir/test/common/autotest_common.sh

: ${SPDK_OCSSD_TEST_RESTORE=1}

function ocssd_kill() {
	rm -f $testdir/.testfile_*
}

declare -A vdids
vdids['qemu']='0x1d1d 0x1f1f'
vdids['intel']='0x8086 0x0a56'

device=
for dev in /sys/bus/pci/devices/*; do
	for _vdid in ${!vdids[@]}; do
		vdid=(${vdids[$_vdid]})
		if [ "$(cat $dev/vendor)" = "${vdid[0]}" ] &&
		   [ "$(cat $dev/device)" = "${vdid[1]}" ]; then
		   device=$(basename $dev)
		   break
		fi
	done

	if [ -n "$device" ]; then
		break
	fi
done

if [ -z "$device" ]; then
	echo "Could not find OCSSD device"
	exit 0
fi

trap "ocssd_kill; exit 1" SIGINT SIGTERM EXIT

timing_enter ocssd
timing_enter fio

run_test suite $testdir/fio.sh $device

timing_exit fio

if [ $SPDK_OCSSD_TEST_RESTORE -eq 1 ]; then
	timing_enter restore
	if [ -f $testdir/.testfile_nvme0.0 ]; then
		uuid=$(cat $testdir/.testfile_nvme0.0)
	fi

	run_test suite $testdir/restore.sh $device $uuid
	timing_exit restore
fi

timing_exit ocssd

trap - SIGINT SIGTERM EXIT
ocssd_kill
