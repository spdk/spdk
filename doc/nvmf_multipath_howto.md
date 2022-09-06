# NVMe-oF Multipath HOWTO {#nvmf_multipath_howto}

This HOWTO provides step-by-step instructions for setting-up a simple SPDK deployment and testing multipath.
It demonstrates configuring path preferences with Asymmetric Namespace Access (ANA), as well as round-robin
path load balancing.

## Build SPDK on both the initiator and target servers

Clone the repo:
~~~{.sh}
git clone https://github.com/spdk/spdk --recursive
~~~

Configure and build SPDK:
~~~{.sh}
cd spdk/
./configure
make -j16
~~~

## Setup hugepages

This should be run once on each server (and after reboots):
~~~{.sh}
cd spdk/
./scripts/setup.sh
~~~

## On target: start and configure SPDK

Start the target in the background and configure it:
~~~{.sh}
cd spdk/
./build/bin/nvmf_tgt -m 0x3 &
./scripts/rpc.py nvmf_create_transport -t tcp -o -u 8192
~~~

Create a subsystem, with `-r` to enable ANA reporting feature:
~~~{.sh}
./scripts/rpc.py nvmf_create_subsystem nqn.2022-02.io.spdk:cnode0 -a -s SPDK00000000000001 -r
~~~

Create and add a malloc block device:
~~~{.sh}
./scripts/rpc.py bdev_malloc_create 64 512 -b Malloc0
./scripts/rpc.py nvmf_subsystem_add_ns nqn.2022-02.io.spdk:cnode0 Malloc0
~~~

Add two listeners, each with a different `IP:port` pair:
~~~{.sh}
./scripts/rpc.py nvmf_subsystem_add_listener -t tcp -a 172.17.1.13 -s 4420 nqn.2022-02.io.spdk:cnode0
./scripts/rpc.py nvmf_subsystem_add_listener -t tcp -a 172.18.1.13 -s 5520 nqn.2022-02.io.spdk:cnode0
~~~

## On initiator: start and configure bdevperf

Launch the bdevperf process in the background:
~~~{.sh}
cd spdk/
./build/examples/bdevperf -m 0x4 -z -r /tmp/bdevperf.sock -q 128 -o 4096 -w verify -t 90 &> bdevperf.log &
~~~

Configure bdevperf and add two paths:
~~~{.sh}
./scripts/rpc.py -s /tmp/bdevperf.sock bdev_nvme_set_options -r -1
./scripts/rpc.py -s /tmp/bdevperf.sock bdev_nvme_attach_controller -b Nvme0 -t tcp -a 172.17.1.13 -s 4420 -f ipv4 -n nqn.2022-02.io.spdk:cnode0 -l -1 -o 10
./scripts/rpc.py -s /tmp/bdevperf.sock bdev_nvme_attach_controller -b Nvme0 -t tcp -a 172.18.1.13 -s 5520 -f ipv4 -n nqn.2022-02.io.spdk:cnode0 -x multipath -l -1 -o 10
~~~

## Launch a bdevperf test

Connect to the RPC socket of the bdevperf process and start the test:
~~~{.sh}
PYTHONPATH=$PYTHONPATH:/root/src/spdk/python ./examples/bdev/bdevperf/bdevperf.py -t 1 -s /tmp/bdevperf.sock perform_tests
~~~

The RPC command will return, leaving the test to run for 90 seconds in the background. On the target server,
observe that only the first path (port) is receiving packets by checking the queues with `ss -t`.

You can view the paths available to the initiator with:
~~~{.sh}
./scripts/rpc.py -s /tmp/bdevperf.sock bdev_nvme_get_io_paths -n Nvme0n1
~~~

## Switching paths

This can be done on the target server by setting the first path's ANA to `non_optimized`:
~~~{.sh}
./scripts/rpc.py nvmf_subsystem_listener_set_ana_state nqn.2022-02.io.spdk:cnode0 -t tcp -a 172.17.1.13 -s 4420 -n non_optimized
~~~

Use `ss -t`  to verify that the traffic has switched to the second path.

## Use round-robin (active_active) path load balancing

First, ensure the ANA for both paths is configured as `optimized` on the target. Then, change the
multipath policy on the initiator to `active_active` (multipath policy is per bdev, so
`bdev_nvme_set_multipath_policy` must be called after `bdev_nvme_attach_controller`):
~~~{.sh}
./scripts/rpc.py -s /tmp/bdevperf.sock bdev_nvme_set_multipath_policy -b Nvme0n1 -p active_active
~~~

Observe with `ss -t` that both connections are receiving traffic (queues build up).
