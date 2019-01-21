set -e

function raid_data_verify() {
	if hash blkdiscard; then
		local nbd=$1
		local rpc_server=$2
		local blksize=$(lsblk -o  LOG-SEC $nbd | grep -v LOG-SEC | cut -d ' ' -f 5)
		local rw_len=$((blksize * rw_blk_num))
		local tmp_file=/tmp/raidrandtest
		local rw_blk_num=4096
		local unmap_blk_offs=(0    1028 321)
		local unmap_blk_nums=(0x80 2035 456)
		local unmap_off
		local unmap_len

		# data write
		dd if=/dev/urandom of=$tmp_file bs=$blksize count=$rw_blk_num
		dd if=$tmp_file of=$nbd bs=$blksize count=$rw_blk_num oflag=direct
		blockdev --flushbufs $nbd

		# confirm random data is written correctly in raid0 device
		cmp -b -n $rw_len $tmp_file $nbd

		for (( i=0; i<${#unmap_blk_offs[@]}; i++ )); do
			unmap_off=$((blksize * ${unmap_blk_offs[$i]}))
			unmap_len=$((blksize * ${unmap_blk_nums[$i]}))

			# Read out data before unmap
			dd if=$nbd of=$tmp_file bs=$blksize count=$rw_blk_num

			# data unmap
			blkdiscard -o $unmap_off -l $unmap_len $nbd
			blockdev --flushbufs $nbd

			# data verify after unmap
			cmp -b -n $rw_len $tmp_file $nbd
		done

		rm $tmp_file
	fi

	return 0
}
