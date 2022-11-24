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

	if ! nvme connect -t $TEST_TRANSPORT -n $nqn -a $ip -s $NVMF_PORT; then
		exit 1
	fi

	waitforserial "$serial"
	nvme_name=$(lsblk -l -o NAME,SERIAL | grep -oP "([\w]*)(?=\s+${serial})")
	nvme_size=$(sec_size_to_bytes $nvme_name)

	echo "${nvme_name}"
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

function test_remove_and_rescan() {
	nvmfappstart -m 0xF

	create_subsystem_and_connect "$@"

	for net_dev in "${!netdev_nvme_dict[@]}"; do
		$rootdir/scripts/fio-wrapper -p nvmf -i 4096 -d 1 -t randrw -r 40 &
		fio_pid=$!
		sleep 3

		nvme_dev=${netdev_nvme_dict[$net_dev]}
		rdma_dev_name=$(get_rdma_device_name $net_dev)
		origin_ip=$(get_ip_address "$net_dev")
		pci_dir=$(get_pci_dir $net_dev)

		if ! $rpc_py nvmf_get_stats | grep "\"name\": \"$rdma_dev_name\""; then
			echo "Device $rdma_dev_name is not registered in tgt".
			exit 1
		fi

		remove_one_nic $net_dev

		for i in $(seq 1 10); do
			if ! $rpc_py nvmf_get_stats | grep "\"name\": \"$rdma_dev_name\""; then
				break
			fi
			if [[ $i == 10 ]]; then
				# failed to remove this device
				exit 1
			fi
			sleep 1
		done

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
	done

	killprocess $nvmfpid
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

	nvmfappstart -m 0xF
	$rpc_py nvmf_create_transport $NVMF_TRANSPORT_OPTS -u 8192

	create_subsystem_and_connect_on_netdev $BOND_NAME

	ib_count=$($rpc_py nvmf_get_stats | grep devices -A 2 | grep -c name)
	echo "IB Count: " $ib_count

	$rootdir/scripts/fio-wrapper -p nvmf -i 4096 -d 1 -t randrw -r 10 &
	fio_pid=$!

	sleep 2
	echo -$nic1 | sudo tee /sys/class/net/${BOND_NAME}/bonding/slaves

	ib_count2=$ib_count
	for i in $(seq 1 10); do
		ib_count2=$($rpc_py nvmf_get_stats | grep devices -A 2 | grep -c name)
		if ((ib_count2 < ib_count)); then
			break
		fi
		sleep 2
	done
	if ((ib_count2 == ib_count)); then
		exit 1
	fi

	# fio will exit when nvmf fin. do not wait here because it may be in D state.
	killprocess $nvmfpid
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
