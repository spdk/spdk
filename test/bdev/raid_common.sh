set -e

function raid_data_verify() {
	if hash blkdiscard; then
		local nbd=$1
		local rpc_server=$2
		local blksize=$(lsblk -o  LOG-SEC $nbd | grep -v LOG-SEC | cut -d ' ' -f 5)
		local unmap_blk_off=1028
		local unmap_blk_num=2035
		local rw_blk_num=4096
		local unmap_off=$((blksize*unmap_blk_off))
		local unmap_len=$((blksize*unmap_blk_num))
		local rw_len=$((blksize*rw_blk_num))
		local tmp_file=/tmp/raidrandtest

		# data write
		dd if=/dev/urandom of=$tmp_file bs=$blksize count=$rw_blk_num
		dd if=$tmp_file of=$nbd bs=$blksize count=$rw_blk_num oflag=direct

		# data write verify
		cmp -b -n $rw_len $tmp_file $nbd

		# data unmap
		blkdiscard -o $unmap_off -l $unmap_len $nbd

		# data unmap verify
		cmp -b -i $unmap_off -n $unmap_len /dev/zero $nbd
		cmp -b -n $unmap_off $tmp_file $nbd
		cmp -b -i $((unmap_len+unmap_off)) -n $((rw_len-unmap_len-unmap_off)) $tmp_file $nbd

		rm $tmp_file
	fi

	return 0
}
