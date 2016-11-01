#!/usr/bin/env bash

NVMF_PORT=4420
NVMF_IP_PREFIX="192.168.100"
NVMF_IP_LEAST_ADDR=8
NVMF_FIRST_TARGET_IP=$NVMF_IP_PREFIX.$NVMF_IP_LEAST_ADDR
RPC_PORT=5260

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
	# could be add other nics, so wrap it
	detect_mellanox_nics
}

function allocate_nic_ips()
{
	let count=$NVMF_IP_LEAST_ADDR
	for nic_type in `ls /sys/class/infiniband`; do
		for nic_name in `ls /sys/class/infiniband/${nic_type}/device/net`; do
			ifconfig $nic_name $NVMF_IP_PREFIX.$count netmask 255.255.255.0 up

			# dump configuration for debug log
			ifconfig $nic_name
			let count=$count+1
		done
	done
}

function nvmfcleanup()
{
	sync
	rmmod nvme-rdma
}

function rdma_device_init()
{
	load_ib_rdma_modules
	detect_rdma_nics
	allocate_nic_ips
}

function rdma_nic_available()
{
	ifconfig | grep -q $NVMF_IP_PREFIX
}
