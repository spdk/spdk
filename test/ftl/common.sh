# Common utility functions to be sourced by the libftl test scripts

function get_chunk_size() {
	$SPDK_EXAMPLE_DIR/identify -r "trtype:PCIe traddr:$1" \
		| grep 'Logical blks per chunk' | sed 's/[^0-9]//g'
}

function get_num_group() {
	$SPDK_EXAMPLE_DIR/identify -r "trtype:PCIe traddr:$1" \
		| grep 'Groups' | sed 's/[^0-9]//g'
}

function get_num_pu() {
	$SPDK_EXAMPLE_DIR/identify -r "trtype:PCIe traddr:$1" \
		| grep 'PUs' | sed 's/[^0-9]//g'
}

function has_separate_md() {
	local md_type
	md_type=$($SPDK_EXAMPLE_DIR/identify -r "trtype:PCIe traddr:$1" \
		| grep 'Metadata Transferred' | cut -d: -f2)
	if [[ "$md_type" =~ Separate ]]; then
		return 0
	else
		return 1
	fi
}

function create_nv_cache_bdev() {
	local name=$1
	local cache_bdf=$2
	local base_bdev=$3

	# use 5% space of base bdev
	local size=$(( $(get_bdev_size $base_bdev)*5/100 ))

	# Create NVMe bdev on specified device and split it so that it has the desired size
	local nvc_bdev
	nvc_bdev=$($rootdir/scripts/rpc.py bdev_nvme_attach_controller -b $name -t PCIe -a $cache_bdf)
	$rootdir/scripts/rpc.py bdev_split_create $nvc_bdev -s $size 1
}

function gen_ftl_nvme_conf() {
	jq . <<- JSON
		{
		  "subsystems": [
		    {
		      "subsystem": "bdev",
		      "config": [
		        {
		          "params": {
		            "nvme_adminq_poll_period_us": 100
		          },
		          "method": "bdev_nvme_set_options"
		        }
		      ]
		    }
		  ]
		}
	JSON
}

# Remove not needed files from shared memory
function remove_shm() {
	echo  Remove shared memory files
	rm -f rm -f /dev/shm/ftl*
	rm -f rm -f /dev/hugepages/ftl*
	rm -f rm -f /dev/shm/spdk*
	rm -f rm -f /dev/shm/iscsi
	rm -f rm -f /dev/hugepages/spdk*
}
