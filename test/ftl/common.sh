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
