Multiple NVMe-oF targets without SPDK event framework
============================================================================================================

# Overview
This example is used to show how to use the nvmf lib. The difference between this example and nvmf target is that
this example use the multi-target. Not all the scenarios can use the multi-target, it depends on the configuration
file. All targets don't have the intersections, they listen to different IP interfaces.
	For example:
	1, multi-target
	configuration file:
	[subsystem1]
	Listen IP 1
	Listen Ip 2
	Namespace 1
	Namespace 2              target 1 -> {subsystem1}
                       ===>
	[subsystem2]             target 2 -> {subsystem2}
	Listen IP 3
	Listen Ip 4
	Namespace 3
	Namespace 4

	2, non-multi-target
	configuration file:
	[subsystem1]
	Listen IP 1
	Listen Ip 2
	Namespace 1
	Namespace 2
                     ===>   target 1 -> {subsystem1, subsystem2}
	[subsystem2]
	Listen Ip 2
	Listen Ip 3
	Namespace 3
	Namespace 4

# Usage:
This example's usage is very similar with nvmf_tgt.
First, we need a configuration file. You can find the example of the configuration file from
	$ROOT_SPDK/etc/spdk/nvmf.conf.in
Then, start the nvmf example targets
	./nvmf -c nvmf_conf.io -m 0xf
Last, start the initiator to connect the nvmf example target and test the IOs
	$ROOT_SPDK/example/nvme/perf/perf -q 64 -o 4095 -w randrw -M 30 -l -t 60 \
	-r "trtype:TCP adrfam:IPv4 traddr:127.0.0.1 trsvcid:4420 subnqn:nqn.2016-06.io.spdk:cnode1"
