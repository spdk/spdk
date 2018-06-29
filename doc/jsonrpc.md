# JSON-RPC Methods {#jsonrpc}

# Overview {#jsonrpc_overview}

SPDK implements a [JSON-RPC 2.0](http://www.jsonrpc.org/specification) server
to allow external management tools to dynamically configure SPDK components.

# App Framework {#jsonrpc_components_app}

## kill_instance {#rpc_kill_instance}

Send a signal to the application.

### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
sig_name                | Required | string      | Signal to send (SIGINT, SIGTERM, SIGQUIT, SIGHUP, or SIGKILL)

### Example

Example request:
~~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "kill_instance",
  "params": {
    "sig_name": "SIGINT"
  }
}
~~~

Example response:
~~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

## context_switch_monitor {#rpc_context_switch_monitor}

Query, enable, or disable the context switch monitoring functionality.

### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
enabled                 | Optional | boolean     | Enable (`true`) or disable (`false`) monitoring (omit this parameter to query the current state)

### Response

The current state of context switch monitoring is returned as a boolean.

### Example

Example request:
~~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "context_switch_monitor",
  "params": {
    "enabled": false
  }
}
~~~

Example response:
~~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": false
}
~~~

## start_subsystem_init {#rpc_start_subsystem_init}

Start initialization of subsystems when it is deferred by starting SPDK application with option -w.
During its deferral some RPCs can be used to set global parameters for subsystems.
This RPC can be called only once.

### Parameters

None

### Response

Status of completion of subsystem initialization is returned as a boolean.

### Example

Example request:
~~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "start_subsystem_init"
}
~~~

Example response:
~~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

## get_rpc_methods {#rpc_get_rpc_methods}

Get an array of supported RPC methods.

### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
current                 | Optional | boolean     | Get an array of RPC methods only callable in the current state.

### Response

The response is an array of supported RPC methods.

### Example

Example request:
~~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "get_rpc_methods"
}
~~~

Example response:
~~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": [
    "start_subsystem_init",
    "get_rpc_methods",
    "get_scsi_devices",
    "get_interfaces",
    "delete_ip_address",
    "add_ip_address",
    "get_nbd_disks",
    "stop_nbd_disk",
    "start_nbd_disk",
    "get_trace_flags",
    "clear_trace_flag",
    "set_trace_flag",
    "get_log_level",
    "set_log_level",
    "get_log_print_level",
    "set_log_print_level",
    "get_iscsi_global_params",
    "target_node_add_lun",
    "get_iscsi_connections",
    "delete_portal_group",
    "add_portal_group",
    "get_portal_groups",
    "delete_target_node",
    "delete_pg_ig_maps",
    "add_pg_ig_maps",
    "construct_target_node",
    "get_target_nodes",
    "delete_initiator_group",
    "delete_initiators_from_initiator_group",
    "add_initiators_to_initiator_group",
    "add_initiator_group",
    "get_initiator_groups",
    "set_iscsi_options",
    "set_bdev_options",
    "set_bdev_qos_limit_iops",
    "delete_bdev",
    "get_bdevs_config",
    "get_bdevs",
    "get_bdevs_iostat",
    "get_subsystem_config",
    "get_subsystems",
    "context_switch_monitor",
    "kill_instance",
    "scan_ioat_copy_engine",
    "construct_virtio_dev",
    "construct_virtio_pci_blk_bdev",
    "construct_virtio_user_blk_bdev",
    "get_virtio_scsi_devs",
    "remove_virtio_scsi_bdev",
    "construct_virtio_pci_scsi_bdev",
    "construct_virtio_user_scsi_bdev",
    "delete_aio_bdev",
    "construct_aio_bdev",
    "destruct_split_vbdev",
    "construct_split_vbdev",
    "bdev_inject_error",
    "delete_error_bdev",
    "construct_error_bdev",
    "construct_passthru_bdev",
    "apply_nvme_firmware",
    "construct_nvme_bdev",
    "construct_null_bdev",
    "delete_malloc_bdev",
    "construct_malloc_bdev",
    "get_lvol_stores",
    "destroy_lvol_bdev",
    "resize_lvol_bdev",
    "decouple_parent_lvol_bdev",
    "inflate_lvol_bdev",
    "rename_lvol_bdev",
    "clone_lvol_bdev",
    "snapshot_lvol_bdev",
    "construct_lvol_bdev",
    "destroy_lvol_store",
    "rename_lvol_store",
    "construct_lvol_store"
  ]
}
~~~

# Block Device Abstraction Layer {#jsonrpc_components_bdev}

## set_bdev_options {#rpc_set_bdev_options}

Set global parameters for the block device (bdev) subsystem.  This RPC may only be called
before subsystems have been initialized.

### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
bdev_io_pool_size       | Optional | number      | Number of spdk_bdev_io structures in shared buffer pool
bdev_io_cache_size      | Optional | number      | Maximum number of spdk_bdev_io structures cached per thread

### Example

Example request:
~~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "set_bdev_options",
  "params": {
    "bdev_io_pool_size": 65536,
    "bdev_io_cache_size": 256
  }
}
~~~

Example response:
~~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

## get_bdevs {#rpc_get_bdevs}

Get information about block devices (bdevs).

### Parameters

The user may specify no parameters in order to list all block devices, or a block device may be
specified by name.

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Optional | string      | Block device name

### Response

The response is an array of objects containing information about the requested block devices.

### Example

Example request:
~~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "get_bdevs",
  "params": {
    "name": "Malloc0"
  }
}
~~~

Example response:
~~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": [
    {
      "name": "Malloc0",
      "product_name": "Malloc disk",
      "block_size": 512,
      "num_blocks": 20480,
      "claimed": false,
      "supported_io_types": {
        "read": true,
        "write": true,
        "unmap": true,
        "write_zeroes": true,
        "flush": true,
        "reset": true,
        "nvme_admin": false,
        "nvme_io": false
      },
      "driver_specific": {}
    }
  ]
}
~~~

## get_bdevs_iostat {#rpc_get_bdevs_iostat}

Get I/O statistics of block devices (bdevs).

### Parameters

The user may specify no parameters in order to list all block devices, or a block device may be
specified by name.

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Optional | string      | Block device name

### Response

The response is an array of objects containing I/O statistics of the requested block devices.

### Example

Example request:
~~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "get_bdevs_iostat",
  "params": {
    "name": "Nvme0n1"
  }
}
~~~

Example response:
~~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": [
    {
      "name": "Nvme0n1",
      "bytes_read": 34051522560,
      "num_read_ops": 8312910,
      "bytes_written": 0,
      "num_write_ops": 0
    }
  ]
}
~~~

## delete_bdev {#rpc_delete_bdev}

Unregister a block device.

### Example

Example request:
~~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "delete_bdev",
  "params": {
    "name": "Malloc0"
  }
}
~~~

Example response:
~~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | Block device name

## set_bdev_qos_limit_iops {#rpc_set_bdev_qos_limit_iops}

Set an IOPS-based quality of service rate limit on a bdev.

### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | Block device name
ios_per_sec             | Required | number      | Number of I/Os per second to allow. 0 means unlimited.

### Example

Example request:
~~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "set_bdev_qos_limit_iops",
  "params": {
    "name": "Malloc0"
    "ios_per_sec": 20000
  }
}
~~~

Example response:
~~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

# NVMe-oF Target {#jsonrpc_components_nvmf_tgt}

## get_nvmf_subsystems method {#rpc_get_nvmf_subsystems}

### Parameters

This method has no parameters.

### Example

Example request:
~~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "get_nvmf_subsystems"
}
~~~

Example response:
~~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": [
    {
      "nqn": "nqn.2014-08.org.nvmexpress.discovery",
      "subtype": "Discovery"
      "listen_addresses": [],
      "hosts": [],
      "allow_any_host": true
    },
    {
      "nqn": "nqn.2016-06.io.spdk:cnode1",
      "subtype": "NVMe",
      "listen_addresses": [
        {
          "trtype": "RDMA",
          "adrfam": "IPv4",
          "traddr": "192.168.0.123",
          "trsvcid": "4420"
        }
      ],
      "hosts": [
        {"nqn": "nqn.2016-06.io.spdk:host1"}
      ],
      "allow_any_host": false,
      "serial_number": "abcdef",
      "namespaces": [
        {"nsid": 1, "name": "Malloc2"},
        {"nsid": 2, "name": "Nvme0n1"}
      ]
    }
  ]
}
~~~

## construct_nvmf_subsystem method {#rpc_construct_nvmf_subsystem}

Construct an NVMe over Fabrics target subsystem.

### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
nqn                     | Required | string      | Subsystem NQN
listen_addresses        | Optional | array       | Array of @ref rpc_construct_nvmf_subsystem_listen_address objects
hosts                   | Optional | array       | Array of strings containing allowed host NQNs. Default: No hosts allowed.
allow_any_host          | Optional | boolean     | Allow any host (`true`) or enforce allowed host whitelist (`false`). Default: `false`.
serial_number           | Required | string      | Serial number of virtual controller
namespaces              | Optional | array       | Array of @ref rpc_construct_nvmf_subsystem_namespace objects. Default: No namespaces.
max_namespaces          | Optional | number      | Maximum number of namespaces that can be attached to the subsystem. Default: 0 (Unlimited)

### listen_address {#rpc_construct_nvmf_subsystem_listen_address}

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
trtype                  | Required | string      | Transport type ("RDMA")
adrfam                  | Required | string      | Address family ("IPv4", "IPv6", "IB", or "FC")
traddr                  | Required | string      | Transport address
trsvcid                 | Required | string      | Transport service ID

### namespace {#rpc_construct_nvmf_subsystem_namespace}

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
nsid                    | Optional | number      | Namespace ID between 1 and 4294967294, inclusive. Default: Automatically assign NSID.
bdev_name               | Required | string      | Name of bdev to expose as a namespace.
nguid                   | Optional | string      | 16-byte namespace globally unique identifier in hexadecimal (e.g. "ABCDEF0123456789ABCDEF0123456789")
eui64                   | Optional | string      | 8-byte namespace EUI-64 in hexadecimal (e.g. "ABCDEF0123456789")
uuid                    | Optional | string      | RFC 4122 UUID (e.g. "ceccf520-691e-4b46-9546-34af789907c5")

### Example

Example request:

~~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "construct_nvmf_subsystem",
  "params": {
    "nqn": "nqn.2016-06.io.spdk:cnode1",
    "listen_addresses": [
      {
        "trtype": "RDMA",
        "adrfam": "IPv4",
        "traddr": "192.168.0.123",
        "trsvcid: "4420"
      }
    ],
    "hosts": [
      "nqn.2016-06.io.spdk:host1",
      "nqn.2016-06.io.spdk:host2"
    ],
    "allow_any_host": false,
    "serial_number": "abcdef",
    "namespaces": [
      {"nsid": 1, "name": "Malloc2"},
      {"nsid": 2, "name": "Nvme0n1"}
    ]
  }
}
~~~

Example response:

~~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

## delete_nvmf_subsystem method {#rpc_delete_nvmf_subsystem}

Delete an existing NVMe-oF subsystem.

### Parameters

Parameter              | Optional | Type        | Description
---------------------- | -------- | ----------- | -----------
nqn                    | Required | string      | Subsystem NQN to delete.

### Example

Example request:

~~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "delete_nvmf_subsystem",
  "params": {
    "nqn": "nqn.2016-06.io.spdk:cnode1"
  }
}
~~~

Example response:

~~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

## nvmf_subsystem_add_listener  method {#rpc_nvmf_subsystem_add_listener}

Add a new listen address to an NVMe-oF subsystem.

### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
nqn                     | Required | string      | Subsystem NQN
listen_address          | Required | object      | @ref rpc_construct_nvmf_subsystem_listen_address object

### Example

Example request:

~~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "nvmf_subsystem_add_listener",
  "params": {
    "nqn": "nqn.2016-06.io.spdk:cnode1",
    "listen_address": {
      "trtype": "RDMA",
      "adrfam": "IPv4",
      "traddr": "192.168.0.123",
      "trsvcid: "4420"
    }
  }
}
~~~

Example response:

~~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

## nvmf_subsystem_add_ns method {#rpc_nvmf_subsystem_add_ns}

Add a namespace to a subsystem. The namespace ID is returned as the result.

### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
nqn                     | Required | string      | Subsystem NQN
namespace               | Required | object      | @ref rpc_construct_nvmf_subsystem_namespace object

### Example

Example request:

~~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "nvmf_subsystem_add_ns",
  "params": {
    "nqn": "nqn.2016-06.io.spdk:cnode1",
    "namespace": {
      "nsid": 3,
      "bdev_name": "Nvme0n1"
    }
  }
}
~~~

Example response:

~~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": 3
}
~~~

## nvmf_subsystem_remove_ns method {#rpc_nvmf_subsystem_remove_ns}

Remove a namespace from a subsystem.

### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
nqn                     | Required | string      | Subsystem NQN
nsid                    | Required | number      | Namespace ID

### Example

Example request:

~~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "nvmf_subsystem_remove_ns",
  "params": {
    "nqn": "nqn.2016-06.io.spdk:cnode1",
    "nsid": 1
  }
}
~~~

Example response:

~~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

## nvmf_subsystem_add_host method {#rpc_nvmf_subsystem_add_host}

Add a host NQN to the whitelist of allowed hosts.

### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
nqn                     | Required | string      | Subsystem NQN
host                    | Required | string      | Host NQN to add to the list of allowed host NQNs

### Example

Example request:

~~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "nvmf_subsystem_add_host",
  "params": {
    "nqn": "nqn.2016-06.io.spdk:cnode1",
    "host": "nqn.2016-06.io.spdk:host1"
  }
}
~~~

Example response:

~~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

## nvmf_subsystem_remove_host method {#rpc_nvmf_subsystem_remove_host}

Remove a host NQN from the whitelist of allowed hosts.

### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
nqn                     | Required | string      | Subsystem NQN
host                    | Required | string      | Host NQN to remove from the list of allowed host NQNs

### Example

Example request:

~~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "nvmf_subsystem_remove_host",
  "params": {
    "nqn": "nqn.2016-06.io.spdk:cnode1",
    "host": "nqn.2016-06.io.spdk:host1"
  }
}
~~~

Example response:

~~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

## nvmf_subsystem_allow_any_host method {#rpc_nvmf_subsystem_allow_any_host}

Configure a subsystem to allow any host to connect or to enforce the host NQN whitelist.

### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
nqn                     | Required | string      | Subsystem NQN
allow_any_host          | Required | boolean     | Allow any host (`true`) or enforce allowed host whitelist (`false`).

### Example

Example request:

~~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "nvmf_subsystem_allow_any_host",
  "params": {
    "nqn": "nqn.2016-06.io.spdk:cnode1",
    "allow_any_host": true
  }
}
~~~

Example response:

~~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

## set_nvmf_target_options {#rpc_set_nvmf_target_options}

Set global parameters for the NVMf target.  This RPC may only be called before subsystems
have been initialized.

### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
max_queue_depth         | Optional | number      | Maximum number of outstanding I/Os per queue
max_qpairs_per_ctrlr    | Optional | number      | Maximum number of SQ and CQ per controller
in_capsule_data_size    | Optional | number      | Maximum number of in-capsule data size
max_io_size             | Optional | number      | Maximum I/O size (bytes)
max_subsystems          | Optional | number      | Maximum number of NVMf subsystems
io_unit_size            | Optional | number      | I/O unit size (bytes)

### Example

Example request:
~~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "set_nvmf_target_options",
  "params": {
    "in_capsule_data_size": 4096,
    "io_unit_size": 131072,
    "max_qpairs_per_ctrlr": 64,
    "max_queue_depth": 128,
    "max_io_size": 131072,
    "max_subsystems": 1024
  }
}
~~~

Example response:
~~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

## set_nvmf_target_config {#rpc_set_nvmf_target_config}

Set global configuration of NVMe-oF target.  This RPC may only be called before subsystems
have been initialized.

### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
acceptor_poll_rate      | Optional | number      | Polling interval of the acceptor for incoming connections (microseconds)

### Example

Example request:
~~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "set_nvmf_target_config",
  "params": {
    "acceptor_poll_rate": 10000
  }
}
~~~

Example response:
~~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~
