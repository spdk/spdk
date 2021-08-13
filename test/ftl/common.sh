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

get_ftl_nvme_dev() {
	# Find device with LBA matching the FTL_BLOCK_SIZE
	local nvmes nvme identify lba

	for nvme in $(nvme_in_userspace); do
		identify=$("$SPDK_EXAMPLE_DIR/identify" -r trtype:pcie -r "traddr:$nvme")
		# TODO: Skip zoned nvme devices - such setup for FTL is currently not
		# supported. See https://github.com/spdk/spdk/issues/1992 for details.
		[[ $identity =~ "NVMe ZNS Zone Report" ]] && continue
		[[ $identify =~ "Current LBA Format:"\ +"LBA Format #"([0-9]+) ]]
		[[ $identify =~ "LBA Format #${BASH_REMATCH[1]}: Data Size:"\ +([0-9]+) ]]
		lba=${BASH_REMATCH[1]}
		((lba && lba % FTL_BLOCK_SIZE == 0)) && nvmes+=("$nvme")
	done
	((${#nvmes[@]} > 0)) || return 1
	printf '%s\n' "${nvmes[@]}"
}

bdev_create_zone() {
	local base_bdev=$1

	# TODO: Consider use of ZNSed nvme controllers
	"$rpc_py" bdev_zone_block_create \
		-b "$ZONE_DEV" \
		-o "$OPTIMAL_OPEN_ZONES" \
		-z "$ZONE_CAPACITY" \
		-n "$base_bdev"
}

bdev_delete_zone() {
	local zone_dev=$1

	# TODO: Consider use of ZNSed nvme controllers
	"$rpc_py" bdev_zone_block_delete "$zone_dev"
}

# Optimal number of zones refers to the number of zones that need to be written at the same
# time in order to maximize drive's write bandwidth.
# ZONE_CAPACITY * FTL_BLOCK_SIZE * OPTIMAL_OPEN_ZONES should be <= size of the drive.
FTL_BLOCK_SIZE=4096
ZONE_CAPACITY=4096
OPTIMAL_OPEN_ZONES=32
ZONE_DEV=zone0

rpc_py=$rootdir/scripts/rpc.py
