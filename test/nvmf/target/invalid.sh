#!/usr/bin/env bash

testdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$testdir/../../..")
source "$rootdir/test/common/autotest_common.sh"
source "$rootdir/test/nvmf/common.sh"

multi_target_rpc=$rootdir/test/nvmf/target/multitarget_rpc.py
rpc=$rootdir/scripts/rpc.py
nqn=nqn.2016-06.io.spdk:cnode
target=foobar
# pre-seed the rng to generate predictive values across different test runs
RANDOM=0

gen_random_s() {
	local length=$1 ll
	# generate ascii table which nvme supports
	local chars=({32..127})
	local string

	for ((ll = 0; ll < length; ll++)); do
		string+="$(echo -e "\x$(printf '%x' "${chars[RANDOM % ${#chars[@]}]}")")"
	done
	# Be nice to rpc.py's arg parser and escape `-` in case it's a first character
	if [[ ${string::1} == "-" ]]; then
		string=${string/-/\\-}
	fi
	echo "$string"
}

nvmftestinit
nvmfappstart -m 0xF

trap 'process_shm --id $NVMF_APP_SHM_ID; nvmftestfini $1; exit 1' SIGINT SIGTERM EXIT

# Attempt to create subsystem with non-existing target
out=$("$rpc" nvmf_create_subsystem -t "$target" "$nqn$RANDOM" 2>&1) && false
[[ $out == *"Unable to find target"* ]]

# Attempt to create subsystem with invalid serial number - inject ASCII char that's
# not in the range (0x20-0x7e) of these supported by the nvme spec.
out=$("$rpc" nvmf_create_subsystem -s "$NVMF_SERIAL$(echo -e "\x1f")" "$nqn$RANDOM" 2>&1) && false
[[ $out == *"Invalid SN"* ]]

# Attempt to create subsystem with invalid model - inject ASCII char that's not in the
# range (0x20-0x7e) of these supported by the nvme spec.
out=$("$rpc" nvmf_create_subsystem -d "SPDK_Controller$(echo -e "\x1f")" "$nqn$RANDOM" 2>&1) && false
[[ $out == *"Invalid MN"* ]]

# Attempt to create subsystem with invalid serial number - exceed SPDK_NVME_CTRLR_SN_LEN (20)
out=$("$rpc" nvmf_create_subsystem -s "$(gen_random_s 21)" "$nqn$RANDOM" 2>&1) && false
[[ $out == *"Invalid SN"* ]]

# Attempt to create subsystem with invalid model - exceed SPDK_NVME_CTRLR_MN_LEN (40)
out=$("$rpc" nvmf_create_subsystem -d "$(gen_random_s 41)" "$nqn$RANDOM" 2>&1) && false
[[ $out == *"Invalid MN"* ]]

# Attempt to delete non-existing subsystem listener
$rpc nvmf_create_transport --trtype "$TEST_TRANSPORT"
$rpc nvmf_create_subsystem $nqn -s SPDK001 -a
if [[ $TEST_TRANSPORT == "TCP" ]]; then
	IP="127.0.0.1"
else
	IP=$(echo "$RDMA_IP_LIST" | head -n 1)
fi
out=$("$rpc" nvmf_subsystem_remove_listener "$nqn" -t "$TEST_TRANSPORT" -a "$IP" -s 4421 2>&1) && false
[[ $out != *"Unable to stop listener."* ]]

# Attempt to create subsystem with invalid controller ID range - outside [1, 0xffef]
out=$("$rpc" nvmf_create_subsystem "$nqn$RANDOM" -i 0 2>&1) && false
[[ $out == *"Invalid cntlid range"* ]]
out=$("$rpc" nvmf_create_subsystem "$nqn$RANDOM" -i 65520 2>&1) && false
[[ $out == *"Invalid cntlid range"* ]]
out=$("$rpc" nvmf_create_subsystem "$nqn$RANDOM" -I 0 2>&1) && false
[[ $out == *"Invalid cntlid range"* ]]
out=$("$rpc" nvmf_create_subsystem "$nqn$RANDOM" -I 65520 2>&1) && false
[[ $out == *"Invalid cntlid range"* ]]

# Attempt to create subsystem with invalid controller ID range - [x, y] where x>y
out=$("$rpc" nvmf_create_subsystem "$nqn$RANDOM" -i 6 -I 5 2>&1) && false
[[ $out == *"Invalid cntlid range"* ]]

# Attempt to delete non-existing target
out=$("$multi_target_rpc" nvmf_delete_target --name "$target" 2>&1) && false
[[ $out == *"The specified target doesn't exist, cannot delete it."* ]]

trap - SIGINT SIGTERM EXIT
nvmftestfini
