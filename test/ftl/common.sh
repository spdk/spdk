# Common utility functions to be sourced by the libftl test scripts

function get_chunk_size() {
	echo $($rootdir/examples/nvme/identify/identify -r "trtype:PCIe traddr:$1" | \
		grep 'Logical blks per chunk' | sed 's/[^0-9]//g')
}

function get_num_group() {
	echo $($rootdir/examples/nvme/identify/identify -r "trtype:PCIe traddr:$1" | \
		grep 'Groups' | sed 's/[^0-9]//g')
}

function get_num_pu() {
	echo $($rootdir/examples/nvme/identify/identify -r "trtype:PCIe traddr:$1" | \
		grep 'PUs' | sed 's/[^0-9]//g')
}

function has_separate_md() {
	local md_type=$($rootdir/examples/nvme/identify/identify -r "trtype:PCIe traddr:$1" | \
		grep 'Metadata Transferred' | cut -d: -f2)
	if [[ "$md_type" =~ Separate ]]; then
		return 0
	else
		return 1
	fi
}

function create_nv_cache_bdev() {
	local name=$1
	local ocssd_bdf=$2
	local cache_bdf=$3
	local num_punits=$4

	local bytes_to_mb=$((1024 * 1024))
	local chunk_size=$(get_chunk_size $ocssd_bdf)

	# We need at least 2 bands worth of data + 1 block
	local size=$((2 * 4096 * $chunk_size * $num_punits + 1))
	# Round the size up to the nearest megabyte
	local size=$((($size + $bytes_to_mb) / $bytes_to_mb))

	# Create NVMe bdev on specified device and split it so that it has the desired size
	local nvc_bdev=$($rootdir/scripts/rpc.py construct_nvme_bdev -b $name -t PCIe -a $cache_bdf)
	$rootdir/scripts/rpc.py construct_split_vbdev $nvc_bdev -s $size 1
}
