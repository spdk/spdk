#!/usr/bin/bash

#set -e

HUGE_2M_PATH=/sys/devices/system/node/node0/hugepages/hugepages-2048kB

pci=$1
mac=$2
#sfnum=$3

function start_prepare {
	echo "start preparation"
	#used for snap dynamic lib
	if [ -z $(echo $LD_LIBRARY_PATH | grep "/usr/local/snap") ]; then
		export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/usr/local/snap/lib
		echo "load snap-rdma lib"
	fi
	
	local hugepages=$(cat $HUGE_2M_PATH/nr_hugepages)
	if [ "$hugepages" -lt 1024 ]; then
		echo 1024 > $HUGE_2M_PATH/nr_hugepages
		echo "setup hugepage config"
	fi
	echo "preparation is done, start..."
}

function stop_app {
	pkill -9 -f "spdk_vrdma"
}

function start_app {
	./spdk_vrdma >/dev/null 2>&1 &
}

function get_sf {
	local sf_array=(` mlnx-sf -a show | grep "RDMA dev" | awk -F " " '{print $3}' `)
	local pci_array=(` mlnx-sf -a show | grep "Parent PCI" | awk '{print $4}' `)
	local mac_array=(` mlnx-sf -a show | grep "HWADDR" | awk '{print $3}' `)

	for(( i=0; i<${#pci_array[@]}; i++ )) do
		if [ -n ${pci_array[i]} -a ${pci_array[i]} = $pci -a ${mac_array[i]} = $mac ]; then
			echo ${sf_array[i]} 
			break
		fi
	done

}

function create_sf {
	local pci=$1
	local mac=$2
	#local sfnum=$2
	#use 6 as sfnum, this sfnum is not used actually
	local cmd="sudo mlnx-sf -a create -d $pci -m $mac --sfnum 6"
	$cmd >/dev/null
	echo $(sudo mlnx-sf -a show | grep "RDMA dev" | awk '{print $3}')
}

function setup_sf {
	local sf=$(get_sf)

	if [ "$sf" ]; then
		echo "sf $sf has been created"
	else
		echo "did not find sf, create sf"
		sf=$(create_sf $pci $mac)
	fi

	local cmd="python3 ./rpc/snap_rpc.py controller_vrdma_configue -d 0 -e mlx5_0 -n $sf"
	echo "setup $sf to controller by rpc cmd (python3 ./rpc/snap_rpc.py controller_vrdma_configue -d 0 -e mlx5_0 -n $sf)"
	cd ../../snap-rdma
	$cmd >/dev/null
	echo "setup sf is done, enjoy vrdma controller"
}

#stop old vrdma app
stop_app
#start
start_prepare
start_app
setup_sf

