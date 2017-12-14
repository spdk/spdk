#!/usr/bin/env bash

set -e

function nbd_start_disks() {
	local rpc_server=$1
	local bdev_list=($2)
	local nbd_list=($3)

	for (( i=0; i<${#nbd_list[@]}; i++ )); do
		$rootdir/scripts/rpc.py -s $rpc_server start_nbd_disk \
		${bdev_list[$i]} ${nbd_list[$i]}
	done
	# Wait for nbd devices ready
	for i in ${nbd_list[@]}; do
		waitfornbd ${i:5}
	done
}

function waitfornbd_exit() {
	nbd_name=$1

	for ((i=1; i<=20; i++)); do
		if grep -q -w $nbd_name /proc/partitions; then
			sleep 0.1
		else
			break
		fi
	done

	return 0
}

function nbd_stop_disks() {
	local rpc_server=$1
	local nbd_list=($2)

	for i in ${nbd_list[@]}; do
		$rootdir/scripts/rpc.py -s $rpc_server stop_nbd_disk $i
	done
	for i in ${nbd_list[@]}; do
		waitfornbd_exit ${i:5}
	done
}

function nbd_get_count() {
	# return = count of spdk nbd devices
	local rpc_server=$1

	nbd_disks_json=`$rootdir/scripts/rpc.py -s $rpc_server get_nbd_disks`
	nbd_disks_name=`echo "${nbd_disks_json}" | jq -r '.[] | .nbd_device'`
	count=`echo "${nbd_disks_name}" | grep -c /dev/nbd || true`
	echo $count
}

function nbd_dd_data_verify() {
	local nbd_list=($1)
	local operation=$2
	local tmp_file=/tmp/nbdrandtest

	if [ "$operation" = "write" ]; then
		# data write
		dd if=/dev/urandom of=$tmp_file bs=4096 count=256
		for i in ${nbd_list[@]}; do
			dd if=$tmp_file of=$i bs=4096 count=256
		done
	elif [ "$operation" = "verify" ]; then
		# data read and verify
		for i in ${nbd_list[@]}; do
			cmp $tmp_file $i -n 1M
		done
		rm $tmp_file
	fi
}

function nbd_rpc_steps() {
	local rpc_server=$1
	local nbd_list=("/dev/nbd0" "/dev/nbd1" "/dev/nbd2" \
					"/dev/nbd3" "/dev/nbd4" "/dev/nbd5")
	local bdev_list=("AIO1" "Malloc2p1" "Malloc2p2" \
					 "Malloc2p3" "Malloc2p4" "Malloc2p5")

	nbd_start_disks $rpc_server "${bdev_list[*]}" "${nbd_list[*]}"
	count=$(nbd_get_count $rpc_server)
	if [ $count -ne ${#nbd_list[@]} ]; then
		return -1
	fi

	nbd_dd_data_verify "${nbd_list[*]}" "write"

	nbd_stop_disks $rpc_server "${nbd_list[*]}"
	count=$(nbd_get_count $rpc_server)
	if [ $count -ne 0 ]; then
		return -1
	fi

	# verify data.
	nbd_start_disks $rpc_server "${bdev_list[*]}" "${nbd_list[*]}"

 	nbd_dd_data_verify "${nbd_list[*]}" "verify"

	return 0
}

function nbd_function_test() {
	if [ $(uname -s) = Linux ] && hash sgdisk; then
		local conf=$1
		local rpc_server=/var/tmp/spdk-nbd.sock

		if [ ! -e $conf ]; then
			return 1
		fi

		modprobe nbd
		$rootdir/test/app/bdev_svc/bdev_svc -r $rpc_server -i 0 -c ${conf} &
		nbd_pid=$!
		echo "Process nbd pid: $nbd_pid"
		waitforlisten $nbd_pid $rpc_server

		nbd_rpc_steps $rpc_server

		killprocess $nbd_pid
	fi

	return 0
}
