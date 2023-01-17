#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2021 Intel Corporation
#  All rights reserved.
#

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/setup/common.sh
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

tgt_core_mask='0x3'
bdevperf_core_mask='0x4'
bdevperf_rpc_sock=/var/tmp/bdevperf.sock
bdevperf_rpc_pid=-1

nvmftestinit

function get_subsystem_nqn() {
	echo nqn.2016-06.io.spdk:system_$1
}

function create_subsystem_and_connect_on_netdev() {
	local -a dev_name

	dev_name=$1
	malloc_name=$dev_name
	nqn=$(get_subsystem_nqn "$dev_name")
	ip=$(get_ip_address "$dev_name")
	serial=SPDK000$dev_name

	MALLOC_BDEV_SIZE=128
	MALLOC_BLOCK_SIZE=512

	$rpc_py bdev_malloc_create $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE -b $malloc_name
	$rpc_py nvmf_create_subsystem $nqn -a -s $serial
	$rpc_py nvmf_subsystem_add_ns $nqn $malloc_name
	$rpc_py nvmf_subsystem_add_listener $nqn -t $TEST_TRANSPORT -a $ip -s $NVMF_PORT

	return 0
}

function create_subsystem_and_connect() {
	local -gA netdev_nvme_dict
	netdev_nvme_dict=()

	$rpc_py nvmf_create_transport $NVMF_TRANSPORT_OPTS -u 8192 "$@"
	for net_dev in $(get_rdma_if_list); do
		netdev_nvme_dict[$net_dev]="$(create_subsystem_and_connect_on_netdev $net_dev)"
	done

	return 0
}

function rescan_pci() {
	echo 1 > /sys/bus/pci/rescan
}

function get_pci_dir() {
	dev_name=$1
	readlink -f /sys/bus/pci/devices/*/net/${dev_name}/device
}

function remove_one_nic() {
	dev_name=$1
	echo 1 > $(get_pci_dir $dev_name)/remove
}

function get_rdma_device_name() {
	dev_name=$1
	ls $(get_pci_dir $dev_name)/infiniband
}

function check_rdma_dev_exists_in_nvmf_tgt() {
	local rdma_dev_name=$1
	$rpc_py nvmf_get_stats | jq -r '.poll_groups[0].transports[].devices[].name' | grep "$rdma_dev_name"
	return $?
}

function get_rdma_dev_count_in_nvmf_tgt() {
	local rdma_dev_name=$1
	$rpc_py nvmf_get_stats | jq -r '.poll_groups[0].transports[].devices | length'
}

function generate_io_traffic_with_bdevperf() {
	local dev_names=("$@")

	mkdir -p $testdir
	$rootdir/build/examples/bdevperf -m $bdevperf_core_mask -z -r $bdevperf_rpc_sock -q 128 -o 4096 -w verify -t 90 &> $testdir/try.txt &
	bdevperf_pid=$!

	trap 'process_shm --id $NVMF_APP_SHM_ID; cat $testdir/try.txt; rm -f $testdir/try.txt; kill -9 $bdevperf_pid; nvmftestfini; exit 1' SIGINT SIGTERM EXIT
	waitforlisten $bdevperf_pid $bdevperf_rpc_sock

	# Create a controller and set multipath behavior
	# bdev_retry_count is set to -1 means infinite reconnects
	$rpc_py -s $bdevperf_rpc_sock bdev_nvme_set_options -r -1

	for dev_name in "${dev_names[@]}"; do
		nqn=$(get_subsystem_nqn $dev_name)
		tgt_ip=$(get_ip_address "$dev_name")

		# -l -1 ctrlr_loss_timeout_sec -1 means infinite reconnects
		# -o 1 reconnect_delay_sec time to delay a reconnect retry is limited to 1 sec
		$rpc_py -s $bdevperf_rpc_sock bdev_nvme_attach_controller -b Nvme_$dev_name -t $TEST_TRANSPORT -a $tgt_ip -s $NVMF_PORT -f ipv4 -n $nqn -l -1 -o 1
	done

	$rootdir/examples/bdev/bdevperf/bdevperf.py -t 120 -s $bdevperf_rpc_sock perform_tests &
	bdevperf_rpc_pid=$!

	sleep 5
}

function stop_bdevperf() {
	wait $bdevperf_rpc_pid

	# NOTE: rdma-core <= v43.0 has memleak bug (fixed in commit 7720071f).
	killprocess $bdevperf_pid || true
	bdevperf_pid=

	cat $testdir/try.txt

	trap - SIGINT SIGTERM EXIT
	rm -f $testdir/try.txt
}

function test_remove_and_rescan() {
	nvmfappstart -m "$tgt_core_mask"

	create_subsystem_and_connect "$@"

	generate_io_traffic_with_bdevperf "${!netdev_nvme_dict[@]}"

	for net_dev in "${!netdev_nvme_dict[@]}"; do
		nvme_dev=${netdev_nvme_dict[$net_dev]}
		rdma_dev_name=$(get_rdma_device_name $net_dev)
		origin_ip=$(get_ip_address "$net_dev")
		pci_dir=$(get_pci_dir $net_dev)

		if ! check_rdma_dev_exists_in_nvmf_tgt "$rdma_dev_name"; then
			echo "Device $rdma_dev_name is not registered in tgt".
			exit 1
		fi

		remove_one_nic $net_dev

		for i in $(seq 1 10); do
			if ! check_rdma_dev_exists_in_nvmf_tgt "$rdma_dev_name"; then
				break
			fi
			if [[ $i == 10 ]]; then
				# failed to remove this device
				exit 1
			fi
			sleep 1
		done

		ib_count_after_remove=$(get_rdma_dev_count_in_nvmf_tgt)

		rescan_pci

		for i in $(seq 1 10); do
			new_net_dev=$(ls ${pci_dir}/net || echo)
			if [[ -z $new_net_dev ]]; then
				sleep 1
			elif [[ $new_net_dev != "$net_dev" ]]; then
				echo "Device name changed after rescan, try rename."
				ip link set $new_net_dev down && ip link set $new_net_dev name $net_dev
				sleep 1
			else
				break
			fi
		done

		if [[ -z $new_net_dev ]]; then
			exit 1
		fi

		ip link set $net_dev up
		if [[ -z $(get_ip_address "$net_dev") ]]; then
			ip addr add $origin_ip/24 dev $net_dev
		fi

		# if rdma device name is renamed, nvmf_get_stats may return an obsoleted name.
		# so we check ib device count here instead of the device name.
		for i in $(seq 1 10); do
			ib_count=$(get_rdma_dev_count_in_nvmf_tgt)
			if ((ib_count > ib_count_after_remove)); then
				break
			fi

			if [[ $i == 10 ]]; then
				# failed to rescan this device
				exit 1
			fi
			sleep 2
		done
	done

	stop_bdevperf

	# NOTE: rdma-core <= v43.0 has memleak bug (fixed in commit 7720071f).
	killprocess $nvmfpid || true
	nvmfpid=

	return 0
}

function check_env_for_test_bonding_slaves() {
	# only test with dual-port CX4/CX5.

	local -gA port_nic_map
	local -g target_nics

	# gather dev with same bus-device.
	for bdf in "${mlx[@]}"; do
		pci_net_devs=("/sys/bus/pci/devices/$bdf/net/"*)
		pci_net_devs=("${pci_net_devs[@]##*/}")

		bd=$(echo ${bdf} | cut -d '.' -f 1)

		port_nic_map[$bd]="${pci_net_devs[*]} ${port_nic_map[$bd]}"
	done

	for x in "${port_nic_map[@]}"; do
		ports=($x)
		if ((${#ports[@]} >= 2)); then
			target_nics=(${ports[@]})
			return 0
		fi
	done

	return 1
}

BOND_NAME="bond_nvmf"
BOND_IP="10.11.11.26"
BOND_MASK="24"

function clean_bond_device() {
	if ip link | grep $BOND_NAME; then
		ip link del $BOND_NAME
	fi
	for net_dev in "${target_nics[@]}"; do
		ip link set $net_dev up
	done
}

function test_bonding_slaves_on_nics() {
	nic1=$1
	nic2=$2

	clean_bond_device
	ip link add $BOND_NAME type bond mode 1
	ip link set $nic1 down && sudo ip link set $nic1 master $BOND_NAME
	ip link set $nic2 down && sudo ip link set $nic2 master $BOND_NAME
	ip link set $BOND_NAME up
	ip addr add ${BOND_IP}/${BOND_MASK} dev $BOND_NAME

	# check slaves here
	slaves=($(cat /sys/class/net/${BOND_NAME}/bonding/slaves))
	if ((${#slaves[@]} != 2)); then
		exit 1
	fi

	# wait ib driver activated on bond device
	sleep 5

	nvmfappstart -m "$tgt_core_mask"
	$rpc_py nvmf_create_transport $NVMF_TRANSPORT_OPTS -u 8192

	create_subsystem_and_connect_on_netdev $BOND_NAME

	ib_count=$(get_rdma_dev_count_in_nvmf_tgt)
	echo "IB Count: " $ib_count

	generate_io_traffic_with_bdevperf $BOND_NAME

	sleep 2
	echo -$nic1 | sudo tee /sys/class/net/${BOND_NAME}/bonding/slaves
	sleep 10
	echo +$nic1 | sudo tee /sys/class/net/${BOND_NAME}/bonding/slaves

	ib_count2=$ib_count
	for i in $(seq 1 10); do
		ib_count2=$(get_rdma_dev_count_in_nvmf_tgt)
		if ((ib_count2 == ib_count)); then
			break
		fi
		sleep 2
	done
	if ((ib_count2 != ib_count)); then
		exit 1
	fi

	stop_bdevperf

	# NOTE: rdma-core <= v43.0 has memleak bug (fixed in commit 7720071f).
	killprocess $nvmfpid || true
	nvmfpid=
	return 0
}

function test_bond_slaves() {
	check_env_for_test_bonding_slaves
	if [[ -z "$target_nics" ]]; then
		echo "No available nic ports to run this test."
		exit 0
	fi
	test_bonding_slaves_on_nics "${target_nics[@]}"
}

run_test "nvmf_device_removal_pci_remove_no_srq" test_remove_and_rescan --no-srq
run_test "nvmf_device_removal_pci_remove" test_remove_and_rescan
# bond slaves case needs lag_master & vport_manager are enabled by mlxconfig
# and not work on CI machine currently.
# run_test "nvmf_device_removal_bond_slaves" test_bond_slaves

nvmftestfini
clean_bond_device
