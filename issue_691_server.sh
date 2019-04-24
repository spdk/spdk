RPC="sudo ./scripts/rpc.py"

$RPC construct_nvme_bdev -b Nvme0 -t PCIe -a 0000:82:00.0
$RPC destroy_lvol_store -l lvs
$RPC construct_lvol_store Nvme0n1 lvs
$RPC construct_lvol_bdev -t -l lvs lvol0 1024
#$RPC construct_lvol_bdev -t -l lvs lvol1 1024
#$RPC construct_lvol_bdev -t -l lvs lvol2 1024

$RPC nvmf_create_transport -t RDMA -u 8192 -p 4 -c 0
$RPC nvmf_subsystem_create nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001
$RPC nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 lvs/lvol0
$RPC nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t rdma -a 192.168.100.101 -s 4420

echo 'Done'
