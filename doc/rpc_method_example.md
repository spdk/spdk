# RPC method example {#rpc_method_example}

Here are some examples of frequently used RPC commands for reference.

## Method for bdev

    +-------------+-------------------+----------------------------------------------------------+
    | bdev class  |function           | steps for example                                        |
    +-------------+-------------------+----------------------------------------------------------+
    | malloc bdev |setup malloc       |./scripts/rpc.py construct_malloc_bdev 64 4096            |
    |             |bdev with rdma     |./scripts/rpc.py nvmf_subsystem_create                    |
    |             |                   |                     -s SPDK00000000000001                |
    |             |                   |                     -a -m 20 nqn.2016-06.io.spdk:cnode1  |
    |             |                   |./scripts/rpc.py nvmf_create_transport -t rdma            |
    |             |-------------------+----------------------------------------------------------+
    |             |create nvmf        |./scripts/rpc.py construct_malloc_bdev 64 4096            |
    |             |subsystem on malloc|./scripts/rpc.py nvmf_subsystem_create nqn.2016-06.io.spdk|
    |             |                   |                     :cnode1 -a -s SPDK00000000000001     |
    |             |                   |./scripts/rpc.py nvmf_subsystem_add_ns nqn.2016-06.io.spdk|
    |             |                   |                     :cnode1 Malloc0                      |
    +-------------+-------------------+----------------------------------------------------------+
    | nvme bdev   |setup nvme bdev    |./scripts/rpc.py nvmf_create_transport -t RDMA            |
    |             |                   |                     -u 8192 -p 4                         |
    |             |with pcie          |./scripts/rpc.py construct_nvme_bdev -b Nvme0 -t pcie     |
    |             |                   |                     ( -f ipv4 optional ) -a 0000:5e:00.0 |
    |             |                   |./scripts/rpc.py nvmf_subsystem_create nqn.2016-06.io.spdk|
    |             |                   |                    :cnode1 a -s SPDK00000000000001       |
    |             |                   |./scripts/rpc.py nvmf_subsystem_add_ns nqn.2016-06.io.spdk|
    |             |                   |                     :cnode1 Nvme0n1                      |
    |             |                   |./scripts/rpc.py nvmf_subsystem_add_listener              |
    |             |                   |                     nqn.2016-06.io.spdk:cnode1 -t rdma   |
    |             |                   |                     -a 192.168.89.11 -s 4420             |
    |             |-------------------+----------------------------------------------------------+
    |             |setup nvme bdev    |./scripts/rpc.py nvmf_create_transport -t RDMA            |
    |             |                   |                     -u 8192 -p 4                         |
    |             |with rdma          |./scripts/rpc.py construct_nvme_bdev -t rdma -f ipv4      |
    |             |                   |                     --a 192.168.89.11 -b Nvme0 -s 4420   |
    |             |                   |                     -n nqn.2016-06.io.spdk:cnode1        |
    |             |                   | ./scripts/rpc.py nvmf_subsystem_create nqn.2016-06.io.   |
    |             |                   |                     spdk:cnode1-a -s SPDK00000000000001  |
    |             |                   |./scripts/rpc.py nvmf_subsystem_add_ns nqn.2016-06.io.spdk|
    |             |                   |                    :cnode1 Nvme0n1                       |
    |             |                   |./scripts/rpc.py nvmf_subsystem_add_listener nqn.2016-06. |
    |             |                   |                    .io.spdk:cnode1 -t rdma               |
    |             |                   |                    -a 192.168.89.11 -s 4420              |
    |             |-------------------+----------------------------------------------------------+
    |             |del nvme controller|./scripts/rpc.py delete_nvme_controller Nvme0             |
    +-------------+-------------------+----------------------------------------------------------+
