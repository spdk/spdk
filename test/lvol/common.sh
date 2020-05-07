MALLOC_SIZE_MB=128
MALLOC_BS=512
AIO_SIZE_MB=400
AIO_BS=4096
LVS_DEFAULT_CLUSTER_SIZE_MB=4
LVS_DEFAULT_CLUSTER_SIZE=$((LVS_DEFAULT_CLUSTER_SIZE_MB * 1024 * 1024))
# reserve some MBs for lvolstore metadata
LVS_DEFAULT_CAPACITY_MB=$((MALLOC_SIZE_MB - LVS_DEFAULT_CLUSTER_SIZE_MB))
LVS_DEFAULT_CAPACITY=$((LVS_DEFAULT_CAPACITY_MB * 1024 * 1024))

function get_bdev_jq() {
	rpc_cmd_simple_data_json bdev "$@"
}

function get_lvs_jq() {
	rpc_cmd_simple_data_json lvs "$@"
}

function check_leftover_devices() {
	leftover_bdevs=$(rpc_cmd bdev_get_bdevs)
	[ "$(jq length <<< "$leftover_bdevs")" == "0" ]
	leftover_lvs=$(rpc_cmd bdev_lvol_get_lvstores)
	[ "$(jq length <<< "$leftover_lvs")" == "0" ]
}

function round_down() {
	local CLUSTER_SIZE_MB=$LVS_DEFAULT_CLUSTER_SIZE_MB
	if [ -n "$2" ]; then
		CLUSTER_SIZE_MB=$2
	fi
	echo $(($1 / CLUSTER_SIZE_MB * CLUSTER_SIZE_MB))
}

function run_fio_test() {
	local file=$1
	local offset=$2
	local size=$3
	local rw=$4
	local pattern=$5
	local extra_params=$6

	local pattern_template="" fio_template=""
	if [[ -n "$pattern" ]]; then
		pattern_template="--do_verify=1 --verify=pattern --verify_pattern=$pattern --verify_state_save=0"
	fi

	fio_template="fio --name=fio_test --filename=$file --offset=$offset --size=$size --rw=$rw --direct=1 $extra_params $pattern_template"
	$fio_template
}

function calc() {
	bc -l <<< "define ceil(x) { scale=0; return(x + (x % 1 > 0))/1 } $1"
}
