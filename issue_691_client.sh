P=192.168.100.101

sudo modprobe nvme-rdma
sudo nvme disconnect -n "nqn.2016-06.io.spdk:cnode1"

sudo nvme discover -t rdma -a $IP -s 4420
sudo nvme connect -t rdma -n "nqn.2016-06.io.spdk:cnode1" -a $IP -s 4420

