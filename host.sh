#!/bin/bash

set -e
trap 'ec=$?; if [ $ec -ne 0 ]; then echo "exit $? due to '\$previous_command'"; fi' EXIT
trap 'previous_command=$this_command; this_command=$BASH_COMMAND' DEBUG

sudo scripts/rpc.py nvmf_create_transport -t TCP
sudo scripts/rpc.py bdev_malloc_create -b Malloc0 200 512
sudo scripts/rpc.py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001 -d SPDK_Controller1
sudo scripts/rpc.py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Malloc0
sudo scripts/rpc.py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t TCP -a 127.0.0.1 -s 4420
sudo build/examples/perf -r 'trtype:TCP adrfam:IPv4 traddr:127.0.0.1 trsvcid:4420 subnqn:nqn.2016-06.io.spdk:cnode1' -q 128 -o 4096 -w write -t 100 -c 0x10

