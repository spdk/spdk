MALLOC_SIZE_MB=128
MALLOC_BS=512
LVS_DEFAULT_CLUSTER_SIZE_MB=4
LVS_DEFAULT_CLUSTER_SIZE=$(( LVS_DEFAULT_CLUSTER_SIZE_MB * 1024 * 1024 ))
# reserve some MBs for lvolstore metadata
LVS_DEFAULT_CAPACITY_MB=$(( MALLOC_SIZE_MB - LVS_DEFAULT_CLUSTER_SIZE_MB ))
LVS_DEFAULT_CAPACITY=$(( LVS_DEFAULT_CAPACITY_MB * 1024 * 1024 ))

function run_test() {
	$@

	leftover_bdevs=$(rpc_cmd get_bdevs)
	[ "$(jq length <<< "$leftover_bdevs")" == "0" ]
	leftover_lvs=$(rpc_cmd get_lvol_stores)
	[ "$(jq length <<< "$leftover_lvs")" == "0" ]
}
