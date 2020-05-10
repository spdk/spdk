set -e

function nbd_start_disks() {
	local rpc_server=$1
	local bdev_list=($2)
	local nbd_list=($3)
	local i

	for ((i = 0; i < ${#nbd_list[@]}; i++)); do
		$rootdir/scripts/rpc.py -s $rpc_server nbd_start_disk ${bdev_list[$i]} ${nbd_list[$i]}
		# Wait for nbd device ready
		waitfornbd $(basename ${nbd_list[$i]})
	done
}

function nbd_start_disks_without_nbd_idx() {
	local rpc_server=$1
	local bdev_list=($2)
	local i
	local nbd_device

	for ((i = 0; i < ${#bdev_list[@]}; i++)); do
		nbd_device=$($rootdir/scripts/rpc.py -s $rpc_server nbd_start_disk ${bdev_list[$i]})
		# Wait for nbd device ready
		waitfornbd $(basename ${nbd_device})
	done
}

function waitfornbd_exit() {
	local nbd_name=$1

	for ((i = 1; i <= 20; i++)); do
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
	local i

	for i in "${nbd_list[@]}"; do
		$rootdir/scripts/rpc.py -s $rpc_server nbd_stop_disk $i
		waitfornbd_exit $(basename $i)
	done
}

function nbd_get_count() {
	# return = count of spdk nbd devices
	local rpc_server=$1

	nbd_disks_json=$($rootdir/scripts/rpc.py -s $rpc_server nbd_get_disks)
	nbd_disks_name=$(echo "${nbd_disks_json}" | jq -r '.[] | .nbd_device')
	count=$(echo "${nbd_disks_name}" | grep -c /dev/nbd || true)
	echo $count
}

function nbd_dd_data_verify() {
	local nbd_list=($1)
	local operation=$2
	local tmp_file=$SPDK_TEST_STORAGE/nbdrandtest

	if [ "$operation" = "write" ]; then
		# data write
		dd if=/dev/urandom of=$tmp_file bs=4096 count=256
		for i in "${nbd_list[@]}"; do
			dd if=$tmp_file of=$i bs=4096 count=256 oflag=direct
		done
	elif [ "$operation" = "verify" ]; then
		# data read and verify
		for i in "${nbd_list[@]}"; do
			cmp -b -n 1M $tmp_file $i
		done
		rm $tmp_file
	fi
}

function nbd_rpc_data_verify() {
	local rpc_server=$1
	local bdev_list=($2)
	local nbd_list=($3)

	nbd_start_disks $rpc_server "${bdev_list[*]}" "${nbd_list[*]}"
	count=$(nbd_get_count $rpc_server)
	if [ $count -ne ${#nbd_list[@]} ]; then
		return 1
	fi

	nbd_dd_data_verify "${nbd_list[*]}" "write"
	nbd_dd_data_verify "${nbd_list[*]}" "verify"

	nbd_stop_disks $rpc_server "${nbd_list[*]}"
	count=$(nbd_get_count $rpc_server)
	if [ $count -ne 0 ]; then
		return 1
	fi

	return 0
}

function nbd_rpc_start_stop_verify() {
	local rpc_server=$1
	local bdev_list=($2)

	nbd_start_disks_without_nbd_idx $rpc_server "${bdev_list[*]}"

	nbd_disks_json=$($rootdir/scripts/rpc.py -s $rpc_server nbd_get_disks)
	nbd_disks_name=($(echo "${nbd_disks_json}" | jq -r '.[] | .nbd_device'))
	nbd_stop_disks $rpc_server "${nbd_disks_name[*]}"

	count=$(nbd_get_count $rpc_server)
	if [ $count -ne 0 ]; then
		return 1
	fi

	return 0
}
