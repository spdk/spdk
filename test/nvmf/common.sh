#!/usr/bin/env bash

NVMF_PORT=4420
NVMF_IP_PREFIX="192.168.100"

if [ -z "$NVMF_IP_LEAST_ADDR" ]; then
	NVMF_IP_LEAST_ADDR=8
fi

if [ -z "$NVMF_APP" ]; then
	NVMF_APP=./app/nvmf_tgt/nvmf_tgt
fi

if [ -z "$NVMF_TEST_CORE_MASK" ]; then
	NVMF_TEST_CORE_MASK=0xFFFF
fi

function load_ib_rdma_modules()
{
	if [ `uname` != Linux ]; then
		return 0
	fi

	modprobe ib_cm
	modprobe ib_core
	modprobe ib_ucm
	modprobe ib_umad
	modprobe ib_uverbs
	modprobe iw_cm
	modprobe rdma_cm
	modprobe rdma_ucm
}


function detect_soft_roce_nics()
{
	if hash rxe_cfg; then
		rxe_cfg start
		rdma_nics=$(get_rdma_if_list)
		all_nics=$(ifconfig -s | awk '{print $1}')
		all_nics=("${all_nics[@]/"Iface"}")
		non_rdma_nics=$(echo "$rdma_nics $all_nics" | sort | uniq -u)
		for nic in $non_rdma_nics; do
			if [[ -d /sys/class/net/${nic}/bridge ]]; then
				continue
			fi
			rxe_cfg add $nic || true
		done
	fi
}

function detect_mellanox_nics()
{
	if ! hash lspci; then
		return 0
	fi

	nvmf_nic_bdfs=`lspci | grep Ethernet | grep Mellanox | awk -F ' ' '{print "0000:"$1}'`
	mlx_core_driver="mlx4_core"
	mlx_ib_driver="mlx4_ib"
	mlx_en_driver="mlx4_en"

	if [ -z "$nvmf_nic_bdfs" ]; then
		return 0
	fi

	# for nvmf target loopback test, suppose we only have one type of card.
	for nvmf_nic_bdf in $nvmf_nic_bdfs
	do
		result=`lspci -vvv -s $nvmf_nic_bdf | grep 'Kernel modules' | awk -F ' ' '{print $3}'`
		if [ "$result" == "mlx5_core" ]; then
			mlx_core_driver="mlx5_core"
			mlx_ib_driver="mlx5_ib"
			mlx_en_driver=""
		fi
		break;
	done

	modprobe $mlx_core_driver
	modprobe $mlx_ib_driver
	if [ -n "$mlx_en_driver" ]; then
		modprobe $mlx_en_driver
	fi

	# The mlx4 driver takes an extra few seconds to load after modprobe returns,
	# otherwise ifconfig operations will do nothing.
	sleep 5
}

function detect_rdma_nics()
{
	do_soft_roce=$1
	detect_mellanox_nics
	if [[ "$do_soft_roce" != "no_soft_roce" ]]; then
		detect_soft_roce_nics
	fi
}

function allocate_nic_ips()
{
	let count=$NVMF_IP_LEAST_ADDR
	for nic_name in $(get_rdma_if_list); do
		ip="$(get_ip_address $nic_name)"
		if [ -z $ip ]; then
			ifconfig $nic_name $NVMF_IP_PREFIX.$count netmask 255.255.255.0 up
			let count=$count+1
		fi
		# dump configuration for debug log
		ifconfig $nic_name
	done
}

function get_available_rdma_ips()
{
	for nic_name in $(get_rdma_if_list); do
		ifconfig $nic_name | grep "inet " | awk '{print $2}'
	done
}

function get_rdma_if_list()
{
	for nic_type in `ls /sys/class/infiniband`; do
		for nic_name in `ls /sys/class/infiniband/${nic_type}/device/net`; do
			echo "$nic_name"
		done
	done
}

function get_ip_address()
{
	interface=$1
	ifconfig $interface | grep "inet " | awk '{print $2}' | sed 's/[^0-9\.]//g'
}

function nvmfcleanup()
{
	sync
	rmmod nvme-rdma
}

function rdma_device_init()
{
	load_ib_rdma_modules
	detect_rdma_nics $1
	allocate_nic_ips
}

function revert_soft_roce()
{
	if hash rxe_cfg; then
		interfaces="$(ifconfig -s | awk '{print $1}')"
		for interface in $interfaces; do
			rxe_cfg remove $interface || true
		done
		rxe_cfg stop || true
	fi
}

function check_ip_is_soft_roce()
{
	IP=$1
	if hash rxe_cfg; then
		dev=$(ip -4 -o addr show | grep $IP | cut -d" " -f2)
		if rxe_cfg | grep $dev; then
			return 0
		else
			return 1
		fi
	else
		return 1
	fi
}
