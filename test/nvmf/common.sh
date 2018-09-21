#!/usr/bin/env bash

NVMF_PORT=4420
NVMF_IP_PREFIX="192.168.100"
NVMF_IP_LEAST_ADDR=8
NVMF_TCP_IP_ADDRESS="127.0.0.1"

if [ -z "$NVMF_APP" ]; then
	NVMF_APP=./app/nvmf_tgt/nvmf_tgt
fi

if [ -z "$NVMF_TEST_CORE_MASK" ]; then
	NVMF_TEST_CORE_MASK=0xFF
fi

function load_ib_rdma_modules()
{
	if [ `uname` != Linux ]; then
		return 0
	fi

	modprobe ib_cm
	modprobe ib_core
	# Newer kernels do not have the ib_ucm module
	modprobe ib_ucm || true
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
		all_nics=$(ip -o link | awk '{print $2}' | cut -d":" -f1)
		non_rdma_nics=$(echo -e "$rdma_nics\n$all_nics" | sort | uniq -u)
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
		echo "No NICs"
		return 0
	fi

	nvmf_nic_bdfs=`lspci | grep Ethernet | grep Mellanox | awk -F ' ' '{print "0000:"$1}'`
	mlx_core_driver="mlx4_core"
	mlx_ib_driver="mlx4_ib"
	mlx_en_driver="mlx4_en"

	if [ -z "$nvmf_nic_bdfs" ]; then
		echo "No NICs"
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
	# otherwise iproute2 operations will do nothing.
	sleep 5
}

function detect_rdma_nics()
{
	nics=$(detect_mellanox_nics)
	if [ "$nics" == "No NICs" ]; then
		detect_soft_roce_nics
	fi
}

function allocate_nic_ips()
{
	let count=$NVMF_IP_LEAST_ADDR
	for nic_name in $(get_rdma_if_list); do
		ip="$(get_ip_address $nic_name)"
		if [ -z $ip ]; then
			ip addr add $NVMF_IP_PREFIX.$count/24 dev $nic_name
			ip link set $nic_name up
			let count=$count+1
		fi
		# dump configuration for debug log
		ip addr show $nic_name
	done
}

function get_available_rdma_ips()
{
	for nic_name in $(get_rdma_if_list); do
		get_ip_address $nic_name
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
	ip -o -4 addr show $interface | awk '{print $4}' | cut -d"/" -f1
}

function nvmfcleanup()
{
	sync
	set +e
	for i in {1..20}; do
		modprobe -v -r nvme-rdma nvme-fabrics
		if [ $? -eq 0 ]; then
			set -e
			return
		fi
		sleep 1
	done
	set -e

	# So far unable to remove the kernel modules. Try
	# one more time and let it fail.
	modprobe -v -r nvme-rdma nvme-fabrics
}

function nvmftestinit()
{
	if [ "$1" == "iso" ]; then
		$rootdir/scripts/setup.sh
		rdma_device_init
	fi
}

function nvmftestfini()
{
	if [ "$1" == "iso" ]; then
		$rootdir/scripts/setup.sh reset
		rdma_device_init
	fi
}

function rdma_device_init()
{
	load_ib_rdma_modules
	detect_rdma_nics
	allocate_nic_ips
}

function revert_soft_roce()
{
	if hash rxe_cfg; then
		interfaces="$(ip -o link | awk '{print $2}' | cut -d":" -f1)"
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
