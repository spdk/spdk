#!/usr/bin/env bash
MALLOC_BDEV_SIZE=128
MALLOC_BLOCK_SIZE=512
LVOL_BDEV_SIZE=10
LVOL_BDEVS_NR=10
spdk_dir="/home/sethhowe/Desktop/Development_Folders/spdk/"
rpc_py="$spdk_dir/scripts/rpc.py"

$spdk_dir/app/nvmf_tgt/nvmf_tgt -m 0x7 > /home/sethhowe/test.txt 2>&1 &
nvmfpid=$!

sleep 5

$rpc_py nvmf_create_transport -t RDMA -u 8192 -p 9

malloc_bdev="$($rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE)"

lv_store="lvs_1"

ls_guid="$($rpc_py construct_lvol_store $malloc_bdev $lv_store -c 524288)"

lvol_bdevs=""
for i in `seq 1 $LVOL_BDEVS_NR`; do
	k=$[$i-1]
	lb_name="$($rpc_py construct_lvol_bdev -u $ls_guid lbd_$i $LVOL_BDEV_SIZE)"
	lvol_bdevs+="$lb_name "
	$rpc_py nvmf_subsystem_create nqn.2016-06.io.spdk:uuid:$lb_name -a -s SPDK$i
	$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:uuid:$lb_name $lb_name
	$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:uuid:$lb_name -t rdma -a 192.168.100.8 -s 4420
	nvme discover -t rdma -a "192.168.100.8" -s "4420"
	nvme connect -t rdma -n "nqn.2016-06.io.spdk:uuid:$lb_name" -a "192.168.100.8" -s "4420"
	nvme list
done

for lb_name in $lvol_bdevs; do
	 nvme disconnect -n "nqn.2016-06.io.spdk:uuid:$lb_name" || true
	 sleep 1
	 $rpc_py destroy_lvol_bdev "$lb_name"
done

$rpc_py destroy_lvol_store -l $lv_store

kill $nvmfpid