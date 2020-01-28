MALLOC_SIZE_MB=128
MALLOC_BS=512
LVS_DEFAULT_CLUSTER_SIZE_MB=4
LVS_DEFAULT_CLUSTER_SIZE=$(( LVS_DEFAULT_CLUSTER_SIZE_MB * 1024 * 1024 ))
# reserve some MBs for lvolstore metadata
LVS_DEFAULT_CAPACITY_MB=$(( MALLOC_SIZE_MB - LVS_DEFAULT_CLUSTER_SIZE_MB ))
LVS_DEFAULT_CAPACITY=$(( LVS_DEFAULT_CAPACITY_MB * 1024 * 1024 ))

function rpc_cmd() {
	$rootdir/scripts/rpc.py "$@"
}

function check_leftover_devices() {
	leftover_bdevs=$(rpc_cmd bdev_get_bdevs)
	[ "$(jq length <<< "$leftover_bdevs")" == "0" ]
	leftover_lvs=$(rpc_cmd bdev_lvol_get_lvstores)
	[ "$(jq length <<< "$leftover_lvs")" == "0" ]
}

function round_down() {
	echo $(( $1 / LVS_DEFAULT_CLUSTER_SIZE_MB * LVS_DEFAULT_CLUSTER_SIZE_MB ))
}

function run_fio_test() {
	file=$1
	offset=$2
	size=$3
	rw=$4
	pattern=$5
	extra_params=""
	if [ -n "$6" ]; then
		extra_params=$6
	fi
	pattern_template=""
	if [ ! $pattern ]; then
		pattern_template="--do_verify=1 --verify=pattern --verify_pattern=$pattern --verify_state_save=0"
	fi
	fio_template="fio --name=fio_test --filename=$file --offset=$offset --size=$size --rw=$rw --direct=1 $extra_params $pattern_template"
	$fio_template
}
