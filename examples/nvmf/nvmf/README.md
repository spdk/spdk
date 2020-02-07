# NVMe-oF target without SPDK event framework

## Overview

This example is used to show how to use the nvmf lib. In this example we want to encourage user
to use RPC cmd so we would only support RPC style.

## Usage

This example's usage is very similar with nvmf_tgt, difference is that you must use the RPC cmd
to setup the nvmf target.

First, start this example app. You can use the -m to specify how many cores you want to use.
The other parameters you can use -h to show.
	./nvmf -m 0xf -r /var/tmp/spdk.sock

Then, you need to use the RPC cmd to config the nvmf target. You can use the -h to get how many
RPC cmd you can use. As this example is about nvmf so I think you can focus on the nvmf cmds and
the bdev cmds.
	./scripts/rpc.py -h

Next, You should use the RPC cmd to setup nvmf target.
	./scripts/rpc.py nvmf_create_transport -t RDMA -g nvmf_example
	./scripts/rpc.py nvmf_create_subsystem -t nvmf_example -s SPDK00000000000001 -a -m 32 nqn.2016-06.io.spdk:cnode1
	./scripts/rpc.py bdev_malloc_create -b Malloc1 128 512
	./scripts/rpc.py nvmf_subsystem_add_ns -t nvmf_example nqn.2016-06.io.spdk:cnode1 Malloc1
	./scripts/rpc.py nvmf_subsystem_add_listener -t rdma -f Ipv4 -a 192.168.0.10 -s 4420 -p nvmf_example nqn.2016-06.io.spdk:cnode1

Last, start the initiator to connect the nvmf example target and test the IOs
	$ROOT_SPDK/example/nvme/perf/perf -q 64 -o 4095 -w randrw -M 30 -l -t 60 \
	-r "trtype:RDMA adrfam:IPv4 traddr:192.168.0.10 trsvcid:4420 subnqn:nqn.2016-06.io.spdk:cnode1"
