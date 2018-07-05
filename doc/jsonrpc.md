# JSON-RPC Methods {#jsonrpc}

# Overview {#jsonrpc_overview}

SPDK implements a [JSON-RPC 2.0](http://www.jsonrpc.org/specification) server
to allow external management tools to dynamically configure SPDK components.

## Parameters

Most of the commands can take parameters. If present, parameter is validated against its domain. If this check fail
whole command will fail with response error message [Invalid params](@ref jsonrpc_error_message).

### Required parameters

These parameters are mandatory. If any required parameter is missing RPC command will fail with proper error response.

### Optional parameters

Those parameters might be omitted. If an optional parameter is present it must be valid otherwise command will fail
proper error response.

## Error response message {#jsonrpc_error_message}

Each error response will contain proper message. As much as possible the message should indicate what went wrong during
command processing.

There is ongoing effort to customize this messages but some RPC methods just return "Invalid parameters" as message body
for any kind of error.

Code   | Description
------ | -----------
-1     | Invalid state - given method exists but it is not callable in [current runtime state](@ref rpc_start_subsystem_init)
-32600 | Invalid request - not compliant with JSON-RPC 2.0 Specification
-32601 | Method not found
-32602 | @ref jsonrpc_invalid_params
-32603 | Internal error for e.g.: errors like out of memory
-32700 | @ref jsonrpc_parser_error

### Parser error {#jsonrpc_parser_error}

Encountered some error during parsing request like:

- the JSON object is malformed
- parameter is too long
- request is too long

### Invalid params {#jsonrpc_invalid_params}

This type of error is most common one. It mean that there is an error while processing the request like:

- Parameters decoding in RPC method handler failed because required parameter is missing.
- Unknown parameter present encountered.
- Parameter type doesn't match expected type e.g.: given number when expected a string.
- Parameter domain check failed.
- Request is valid but some other error occurred during processing request. If possible message explains the error reason nature.

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

Name                    | Type        | Description
----------------------- | ----------- | -----------
enabled                 | boolean     | The current state of context switch monitoring

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
  "result": {
    "enabled": false
  }
}
~~~

## start_subsystem_init {#rpc_start_subsystem_init}

Start initialization of SPDK subsystems when it is deferred by starting SPDK application with option -w.
During its deferral some RPCs can be used to set global parameters for SPDK subsystems.
This RPC can be called only once.

### Parameters

This method has no parameters.

### Response

Completion status of SPDK subsystem initialization is returned as a boolean.

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
    "set_bdev_qos_limit",
    "delete_bdev",
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
    "remove_virtio_bdev",
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
    "delete_nvme_controller",
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

## get_subsystems {#rpc_get_subsystems}

Get an array of name and dependency relationship of SPDK subsystems in initialization order.

### Parameters

None

### Response

The response is an array of name and dependency relationship of SPDK subsystems in initialization order.

### Example

Example request:
~~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "get_subsystems"
}
~~~

Example response:
~~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": [
    {
      "subsystem": "copy",
      "depends_on": []
    },
    {
      "subsystem": "interface",
      "depends_on": []
    },
    {
      "subsystem": "net_framework",
      "depends_on": [
        "interface"
      ]
    },
    {
      "subsystem": "bdev",
      "depends_on": [
        "copy"
      ]
    },
    {
      "subsystem": "nbd",
      "depends_on": [
        "bdev"
      ]
    },
    {
      "subsystem": "nvmf",
      "depends_on": [
        "bdev"
      ]
    },
    {
      "subsystem": "scsi",
      "depends_on": [
        "bdev"
      ]
    },
    {
      "subsystem": "vhost",
      "depends_on": [
        "scsi"
      ]
    },
    {
      "subsystem": "iscsi",
      "depends_on": [
        "scsi"
      ]
    }
  ]
}
~~~

## get_subsystem_config {#rpc_get_subsystem_config}

Get current configuration of the specified SPDK subsystem

### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | SPDK subsystem name

### Response

The response is current configuration of the specified SPDK subsystem.
Null is returned if it is not retrievable by the get_subsystem_config method and empty array is returned if it is empty.

### Example

Example request:
~~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "get_subsystem_config",
  "params": {
    "name": "bdev"
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
      "params": {
        "base_bdev": "Malloc2",
        "split_size_mb": 0,
        "split_count": 2
      },
      "method": "construct_split_vbdev"
    },
    {
      "params": {
        "trtype": "PCIe",
        "name": "Nvme1",
        "traddr": "0000:01:00.0"
      },
      "method": "construct_nvme_bdev"
    },
    {
      "params": {
        "trtype": "PCIe",
        "name": "Nvme2",
        "traddr": "0000:03:00.0"
      },
      "method": "construct_nvme_bdev"
    },
    {
      "params": {
        "block_size": 512,
        "num_blocks": 131072,
        "name": "Malloc0",
        "uuid": "913fc008-79a7-447f-b2c4-c73543638c31"
      },
      "method": "construct_malloc_bdev"
    },
    {
      "params": {
        "block_size": 512,
        "num_blocks": 131072,
        "name": "Malloc1",
        "uuid": "dd5b8f6e-b67a-4506-b606-7fff5a859920"
      },
      "method": "construct_malloc_bdev"
    }
  ]
}
~~~

# Block Device Abstraction Layer {#jsonrpc_components_bdev}

## set_bdev_options {#rpc_set_bdev_options}

Set global parameters for the block device (bdev) subsystem.  This RPC may only be called
before SPDK subsystems have been initialized.

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
      "tick_rate": 2200000000
    },
    {
      "name": "Nvme0n1",
      "bytes_read": 36864,
      "num_read_ops": 2,
      "bytes_written": 0,
      "num_write_ops": 0,
      "read_latency_ticks": 178904,
      "write_latency_ticks": 0,
      "queue_depth_polling_period": 2,
      "queue_depth": 0,
      "io_time": 0,
      "weighted_io_time": 0
    }
  ]
}
~~~

## delete_bdev {#rpc_delete_bdev}

Unregister a block device.

### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | Block device name

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

## set_bdev_qos_limit {#rpc_set_bdev_qos_limit}

Set the quality of service rate limit on a bdev.

### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | Block device name
rw_ios_per_sec          | Optional | number      | Number of R/W I/Os per second to allow. 0 means unlimited.
rw_mbytes_per_sec       | Optional | number      | Number of R/W megabytes per second to allow. 0 means unlimited.

### Example

Example request:
~~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "set_bdev_qos_limit",
  "params": {
    "name": "Malloc0"
    "rw_ios_per_sec": 20000
    "rw_mbytes_per_sec": 100
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

## construct_malloc_bdev {#rpc_construct_malloc_bdev}

Construct @ref bdev_config_malloc

### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Optional | string      | Bdev name to use
block_size              | Required | number      | Block size in bytes -must be multiple of 512
num_blocks              | Required | number      | Number of blocks
uuid                    | Optional | string      | UUID of new bdev

### Result

Name of newly created bdev.

### Example

Example request:

~~~
{
  "params": {
    "block_size": 4096,
    "num_blocks": 16384,
    "name": "Malloc0",
    "uuid": "2b6601ba-eada-44fb-9a83-a20eb9eb9e90"
  },
  "jsonrpc": "2.0",
  "method": "construct_malloc_bdev",
  "id": 1
}
~~~

Example response:

~~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": "Malloc0"
}
~~~

## delete_malloc_bdev {#rpc_delete_malloc_bdev}

Delete @ref bdev_config_malloc

### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | Bdev name

### Example

Example request:

~~~
{
  "params": {
    "name": "Malloc0"
  },
  "jsonrpc": "2.0",
  "method": "delete_malloc_bdev",
  "id": 1
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

## construct_null_bdev {#rpc_construct_null_bdev}

Construct @ref bdev_config_null

### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Optional | string      | Bdev name to use
block_size              | Required | number      | Block size in bytes
num_blocks              | Required | number      | Number of blocks
uuid                    | Optional | string      | UUID of new bdev

### Result

Name of newly created bdev.

### Example

Example request:

~~~
{
  "params": {
    "block_size": 4096,
    "num_blocks": 16384,
    "name": "Null0",
    "uuid": "2b6601ba-eada-44fb-9a83-a20eb9eb9e90"
  },
  "jsonrpc": "2.0",
  "method": "construct_null_bdev",
  "id": 1
}
~~~

Example response:

~~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": "Null0"
}
~~~

## delete_null_bdev {#rpc_delete_null_bdev}

Delete @ref bdev_config_null.

### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | Bdev name

### Example

Example request:

~~~
{
  "params": {
    "name": "Null0"
  },
  "jsonrpc": "2.0",
  "method": "delete_null_bdev",
  "id": 1
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

## construct_aio_bdev {#rpc_construct_aio_bdev}

Construct @ref bdev_config_aio.

### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | Bdev name to use
filename                | Required | number      | Path to device or file
block_size              | Optional | number      | Block size in bytes

### Result

Name of newly created bdev.

### Example

Example request:

~~~
{
  "params": {
    "block_size": 4096,
    "name": "Aio0",
    "filename": "/tmp/aio_bdev_file"
  },
  "jsonrpc": "2.0",
  "method": "construct_aio_bdev",
  "id": 1
}
~~~

Example response:

~~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": "Aio0"
}
~~~

## delete_aio_bdev {#rpc_delete_aio_bdev}

Delete @ref bdev_config_aio.

### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | Bdev name

### Example

Example request:

~~~
{
  "params": {
    "name": "Aio0"
  },
  "jsonrpc": "2.0",
  "method": "delete_aio_bdev",
  "id": 1
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

## set_bdev_nvme_options {#rpc_set_bdev_nvme_options}

Set global parameters for all bdev NVMe. This RPC may only be called before SPDK subsystems have been initialized.

### Parameters

Name                       | Optional | Type        | Description
-------------------------- | -------- | ----------- | -----------
action_on_timeout          | Optional | string      | Action to take on command time out: none, reset or abort
timeout_us                 | Optional | number      | Timeout for each command, in microseconds. If 0, don't track timeouts
retry_count                | Optional | number      | The number of attempts per I/O before an I/O fails
nvme_adminq_poll_period_us | Optional | number      | How often the admin queue is polled for asynchronous events in microsecond

### Example

Example request:

~~~
request:
{
  "params": {
    "retry_count": 5,
    "nvme_adminq_poll_period_us": 2000,
    "timeout_us": 10000000,
    "action_on_timeout": "reset"
  },
  "jsonrpc": "2.0",
  "method": "set_bdev_nvme_options",
  "id": 1
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

## set_bdev_nvme_hotplug {#rpc_set_bdev_nvme_hotplug}

Change settings of the NVMe hotplug feature. If enabled, PCIe NVMe bdevs will be automatically discovered on insertion
and deleted on removal.

### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
enabled                 | Required | string      | True to enable, false to disable
period_us               | Optional | number      | How often to poll for hot-insert and hot-remove events. Values: 0 - reset/use default or 1 to 10000000.

### Example

Example request:

~~~
request:
{
  "params": {
    "enabled": true,
    "period_us": 2000
  },
  "jsonrpc": "2.0",
  "method": "set_bdev_nvme_hotplug",
  "id": 1
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

## construct_nvme_bdev {#rpc_construct_nvme_bdev}

Construct @ref bdev_config_nvme

### Result

Array of names of newly created bdevs.

### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | Bdev name
trtype                  | Required | string      | NVMe-oF target trtype: rdma or pcie
traddr                  | Required | string      | NVMe-oF target address: ip or BDF
adrfam                  | Optional | string      | NVMe-oF target adrfam: ipv4, ipv6, ib, fc, intra_host
trsvcid                 | Optional | string      | NVMe-oF target trsvcid: port number
subnqn                  | Optional | string      | NVMe-oF target subnqn
hostnqn                 | Optional | string      | NVMe-oF target hostnqn

### Example

Example request:

~~~
{
  "params": {
    "trtype": "pcie",
    "name": "Nvme0",
    "traddr": "0000:0a:00.0"
  },
  "jsonrpc": "2.0",
  "method": "construct_nvme_bdev",
  "id": 1
}
~~~

Example response:

~~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": [
    "Nvme0n1"
  ]
}
~~~

## delete_nvme_controller {#rpc_delete_nvme_controller}

Delete NVMe controller.

### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | Controller name

### Example

Example requests:

~~~
{
  "params": {
    "name": "Nvme0"
  },
  "jsonrpc": "2.0",
  "method": "delete_nvme_controller",
  "id": 1
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

## construct_rbd_bdev {#rpc_construct_rbd_bdev}

Construct @ref bdev_config_rbd bdev

This method is available only if SPDK was build with Ceph RBD support.

### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Optional | string      | Bdev name
pool_name               | Required | string      | Pool name
rbd_name                | Required | string      | Image name
block_size              | Required | number      | Block size

### Result

Name of newly created bdev.

### Example

Example request:

~~~
{
  "params": {
    "pool_name": "rbd",
    "rbd_name": "foo",
    "block_size": 4096
  },
  "jsonrpc": "2.0",
  "method": "construct_rbd_bdev",
  "id": 1
}
~~~

Example response:

~~~
response:
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": "Ceph0"
}
~~~

## delete_rbd_bdev {#rpc_delete_rbd_bdev}

Delete @ref bdev_config_rbd bdev

This method is available only if SPDK was build with Ceph RBD support.

### Result

`true` if bdev with provided name was deleted or `false` otherwise.

### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | Bdev name

### Example

Example request:

~~~
{
  "params": {
    "name": "Rbd0"
  },
  "jsonrpc": "2.0",
  "method": "delete_rbd_bdev",
  "id": 1
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

## construct_error_bdev {#rpc_construct_error_bdev}

Construct error bdev.

### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
base_name               | Required | string      | Base bdev name

### Example

Example request:

~~~
{
  "params": {
    "base_name": "Malloc0"
  },
  "jsonrpc": "2.0",
  "method": "construct_error_bdev",
  "id": 1
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

## delete_error_bdev {#rpc_delete_error_bdev}

Delete error bdev

### Result

`true` if bdev with provided name was deleted or `false` otherwise.

### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | Error bdev name

### Example

Example request:

~~~
{
  "params": {
    "name": "EE_Malloc0"
  },
  "jsonrpc": "2.0",
  "method": "delete_error_bdev",
  "id": 1
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

## construct_iscsi_bdev {#rpc_construct_iscsi_bdev}

Connect to iSCSI target and create bdev backed by this connection.

This method is available only if SPDK was build with iSCSI initiator support.

### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | Bdev name
initiator_iqn           | Required | string      | IQN name used during connection
url                     | Required | string      | iSCSI resource URI

### Result

Name of newly created bdev.

### Example

Example request:

~~~
{
  "params": {
    "url": "iscsi://127.0.0.1/iqn.2016-06.io.spdk:disk1/0",
    "initiator_iqn": "iqn.2016-06.io.spdk:init",
    "name": "iSCSI0"
  },
  "jsonrpc": "2.0",
  "method": "construct_iscsi_bdev",
  "id": 1
}
~~~

Example response:

~~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": "iSCSI0"
}
~~~

## delete_iscsi_bdev {#rpc_delete_iscsi_bdev}

Delete iSCSI bdev and terminate connection to target.

This method is available only if SPDK was built with iSCSI initiator support.

### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | Bdev name

### Example

Example request:

~~~
{
  "params": {
    "name": "iSCSI0"
  },
  "jsonrpc": "2.0",
  "method": "delete_iscsi_bdev",
  "id": 1
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


## create_pmem_pool {#rpc_create_pmem_pool}

Create a @ref bdev_config_pmem blk pool file. It is equivalent of following `pmempool create` command:

~~~
pmempool create -s $((num_blocks * block_size)) blk $block_size $pmem_file
~~~

This method is available only if SPDK was built with PMDK support.

### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
pmem_file               | Required | string      | Path to new pmem file
num_blocks              | Required | number      | Number of blocks
block_size              | Required | number      | Size of each block in bytes

### Example

Example request:

~~~
{
  "params": {
    "block_size": 512,
    "num_blocks": 131072,
    "pmem_file": "/tmp/pmem_file"
  },
  "jsonrpc": "2.0",
  "method": "create_pmem_pool",
  "id": 1
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

## pmem_pool_info {#rpc_pmem_pool_info}

Retrieve basic information about PMDK memory pool.

This method is available only if SPDK was built with PMDK support.

### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
pmem_file               | Required | string      | Path to existing pmem file

### Result

Array of objects describing memory pool:

Name                    | Type        | Description
----------------------- | ----------- | -----------
num_blocks              | number      | Number of blocks
block_size              | number      | Size of each block in bytes

### Example

Example request:

~~~
request:
{
  "params": {
    "pmem_file": "/tmp/pmem_file"
  },
  "jsonrpc": "2.0",
  "method": "pmem_pool_info",
  "id": 1
}
~~~

Example response:

~~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": [
    {
      "block_size": 512,
      "num_blocks": 129728
    }
  ]
}
~~~

## delete_pmem_pool {#rpc_delete_pmem_pool}

Delete pmem pool by removing file `pmem_file`. This method will fail if `pmem_file` is not a
valid pmem pool file.

This method is available only if SPDK was built with PMDK support.

### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
pmem_file               | Required | string      | Path to new pmem file

### Example

Example request:

~~~
{
  "params": {
    "pmem_file": "/tmp/pmem_file"
  },
  "jsonrpc": "2.0",
  "method": "delete_pmem_pool",
  "id": 1
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

## construct_pmem_bdev {#rpc_construct_pmem_bdev}

Construct @ref bdev_config_pmem bdev.

This method is available only if SPDK was built with PMDK support.

### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | Bdev name
pmem_file               | Required | string      | Path to existing pmem blk pool file

### Result

Name of newly created bdev.

### Example

Example request:

~~~
{
  "params": {
    "pmem_file": "/tmp/pmem_file",
    "name": "Pmem0"
  },
  "jsonrpc": "2.0",
  "method": "construct_pmem_bdev",
  "id": 1
}
~~~

Example response:

~~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": "Pmem0"
}
~~~

## delete_pmem_bdev {#rpc_delete_pmem_bdev}

Delete @ref bdev_config_pmem bdev. This call will not remove backing pool files.

This method is available only if SPDK was built with PMDK support.

### Result

`true` if bdev with provided name was deleted or `false` otherwise.

### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | Bdev name

### Example

Example request:

~~~
{
  "params": {
    "name": "Pmem0"
  },
  "jsonrpc": "2.0",
  "method": "delete_pmem_bdev",
  "id": 1
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

## construct_passthru_bdev {#rpc_construct_passthru_bdev}

Create passthru bdev. This bdev type redirects all IO to it's base bdev. It has no other purpose than being an example
and a starting point in development of new bdev type.

### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
passthru_bdev_name      | Required | string      | Bdev name
base_bdev_name          | Required | string      | Base bdev name

### Result

Name of newly created bdev.

### Example

Example request:

~~~
{
  "params": {
    "base_bdev_name": "Malloc0",
    "passthru_bdev_name": "Passsthru0"
  },
  "jsonrpc": "2.0",
  "method": "construct_passthru_bdev",
  "id": 1
}
~~~

Example response:

~~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": "Passsthru0"
}
~~~

## delete_passthru_bdev {#rpc_delete_passthru_bdev}

Delete passthru bdev.

### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | Bdev name

### Example

Example request:

~~~
{
  "params": {
    "name": "Passsthru0"
  },
  "jsonrpc": "2.0",
  "method": "delete_passthru_bdev",
  "id": 1
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

## construct_virtio_dev {#rpc_construct_virtio_dev}

Create new initiator @ref bdev_config_virtio_scsi or @ref bdev_config_virtio_blk and expose all found bdevs.

### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | Virtio SCSI base bdev name or Virtio Blk bdev name
trtype                  | Required | string      | Virtio target trtype: pci or user
traddr                  | Required | string      | target address: BDF or UNIX socket file path
dev_type                | Required | string      | Virtio device type: blk or scsi
vq_count                | Optional | number      | Number of queues this controller will utilize (default: 1)
vq_size                 | Optional | number      | Size of each queue. Must be power of 2. (default: 512)

In case of Virtio SCSI the `name` parameter will be base name for new created bdevs. For Virtio Blk `name` will be the
name of created bdev.

`vq_count` and `vq_size` parameters are valid only if `trtype` is `user`.

### Result

Array of names of newly created bdevs.

### Example

Example request:

~~~
{
  "params": {
    "name": "VirtioScsi0",
    "trtype": "user",
    "vq_size": 128,
    "dev_type": "scsi",
    "traddr": "/tmp/VhostScsi0",
    "vq_count": 4
  },
  "jsonrpc": "2.0",
  "method": "construct_virtio_dev",
  "id": 1
}
~~~

Example response:

~~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": ["VirtioScsi0t2", "VirtioScsi0t4"]
}
~~~

## construct_virtio_user_scsi_bdev {#rpc_construct_virtio_user_scsi_bdev}

This is legacy RPC method. It is equivalent of @ref rpc_construct_virtio_dev with `trtype` set to `user` and `dev_type` set to `scsi`.

Because it will be deprecated soon it is intentionally undocumented.


## construct_virtio_pci_scsi_bdev {#rpc_construct_virtio_pci_scsi_bdev}

This is legacy RPC method. It is equivalent of @ref rpc_construct_virtio_dev with `trtype` set to `pci` and `dev_type` set to `scsi`.

Because it will be deprecated soon it is intentionally undocumented.

## construct_virtio_user_blk_bdev {#rpc_construct_virtio_user_blk_bdev}

This is legacy RPC method. It is equivalent of @ref rpc_construct_virtio_dev with `trtype` set to `user` and `dev_type` set to `blk`.

Because it will be deprecated soon it is intentionally undocumented.


## construct_virtio_pci_blk_bdev {#rpc_construct_virtio_pci_blk_bdev}

This is legacy RPC method. It is equivalent of @ref rpc_construct_virtio_dev with `trtype` set to `pci` and `dev_type` set to `blk`.

Because it will be deprecated soon it is intentionally undocumented.

## get_virtio_scsi_devs {#rpc_get_virtio_scsi_devs}

Show information about all available Virtio SCSI devices.

### Parameters

This method has no parameters.

### Result

Array of Virtio SCSI information objects.

### Example

Example request:

~~~
{
  "jsonrpc": "2.0",
  "method": "get_virtio_scsi_devs",
  "id": 1
}
~~~

Example response:

~~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": [
    {
      "name": "VirtioScsi0",
      "virtio": {
          "vq_size": 128,
          "vq_count": 4,
          "type": "user",
          "socket": "/tmp/VhostScsi0"
      }
    }
  ]
}
~~~

## remove_virtio_bdev {#rpc_remove_virtio_bdev}

Remove a Virtio device. This command can be used to remove any type of virtio device.

### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | Virtio name

### Example

Example request:

~~~
{
  "params": {
    "name": "VirtioUser0"
  },
  "jsonrpc": "2.0",
  "method": "remove_virtio_bdev",
  "id": 1
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

# iSCSI Target {#jsonrpc_components_iscsi_tgt}

## set_iscsi_options method {#rpc_set_iscsi_options}

Set global parameters for iSCSI targets.

This RPC may only be called before SPDK subsystems have been initialized. This RPC can be called only once.

### Parameters

Name                        | Optional | Type    | Description
--------------------------- | -------- | ------- | -----------
auth_file                   | Optional | string  | Path to CHAP shared secret file (default: "")
node_base                   | Optional | string  | Prefix of the name of iSCSI target node (default: "iqn.2016-06.io.spdk")
nop_timeout                 | Optional | number  | Timeout in seconds to nop-in request to the initiator (default: 60)
nop_in_interval             | Optional | number  | Time interval in secs between nop-in requests by the target (default: 30)
disable_chap                | Optional | boolean | CHAP for discovery session should be disabled (default: `false`)
require_chap                | Optional | boolean | CHAP for discovery session should be required (default: `false`)
mutual_chap                 | Optional | boolean | CHAP for discovery session should be unidirectional (`false`) or bidirectional (`true`) (default: `false`)
chap_group                  | Optional | number  | CHAP group ID for discovery session (default: 0)
max_sessions                | Optional | number  | Maximum number of sessions in the host (default: 128)
max_queue_depth             | Optional | number  | Maximum number of outstanding I/Os per queue (default: 64)
max_connections_per_session | Optional | number  | Session specific parameter, MaxConnections (default: 2)
default_time2wait           | Optional | number  | Session specific parameter, DefaultTime2Wait (default: 2)
default_time2retain         | Optional | number  | Session specific parameter, DefaultTime2Retain (default: 20)
first_burst_length          | Optional | number  | Session specific parameter, FirstBurstLength (default: 8192)
immediate_data              | Optional | boolean | Session specific parameter, ImmediateData (default: `true`)
error_recovery_level        | Optional | number  | Session specific parameter, ErrorRecoveryLevel (default: 0)
allow_duplicated_isid       | Optional | boolean | Allow duplicated initiator session ID (default: `false`)
min_connections_per_core    | Optional | number  | Allocation unit of connections per core (default: 4)

To load CHAP shared secret file, its path is required to specify explicitly in the parameter `auth_file`.

Parameters `disable_chap` and `require_chap` are mutually exclusive. Parameters `no_discovery_auth`, `req_discovery_auth`, `req_discovery_auth_mutual`, and `discovery_auth_group` are still available instead of `disable_chap`, `require_chap`, `mutual_chap`, and `chap_group`, respectivey but will be removed in future releases.

### Example

Example request:

~~~
{
  "params": {
    "allow_duplicated_isid": true,
    "default_time2retain": 60,
    "first_burst_length": 8192,
    "immediate_data": true,
    "node_base": "iqn.2016-06.io.spdk",
    "max_sessions": 128,
    "nop_timeout": 30,
    "nop_in_interval": 30,
    "auth_file": "/usr/local/etc/spdk/auth.conf",
    "disable_chap": true,
    "default_time2wait": 2
  },
  "jsonrpc": "2.0",
  "method": "set_iscsi_options",
  "id": 1
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

## get_iscsi_global_params method {#rpc_get_iscsi_global_params}

Show global parameters of iSCSI targets.

### Parameters

This method has no parameters.

### Example

Example request:

~~~
request:
{
  "jsonrpc": "2.0",
  "method": "get_iscsi_global_params",
  "id": 1
}
~~~

Example response:

~~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "allow_duplicated_isid": true,
    "default_time2retain": 60,
    "first_burst_length": 8192,
    "immediate_data": true,
    "node_base": "iqn.2016-06.io.spdk",
    "mutual_chap": false,
    "nop_in_interval": 30,
    "chap_group": 0,
    "max_connections_per_session": 2,
    "max_queue_depth": 64,
    "nop_timeout": 30,
    "max_sessions": 128,
    "error_recovery_level": 0,
    "auth_file": "/usr/local/etc/spdk/auth.conf",
    "min_connections_per_core": 4,
    "disable_chap": true,
    "default_time2wait": 2,
    "require_chap": false
  }
}
~~~
## set_iscsi_discovery_auth method {#rpc_set_iscsi_discovery_auth}

Set CHAP authentication for sessions dynamically.

### Parameters

Name                        | Optional | Type    | Description
--------------------------- | -------- | --------| -----------
disable_chap                | Optional | boolean | CHAP for discovery session should be disabled (default: `false`)
require_chap                | Optional | boolean | CHAP for discovery session should be required (default: `false`)
mutual_chap                 | Optional | boolean | CHAP for discovery session should be unidirectional (`false`) or bidirectional (`true`) (default: `false`)
chap_group                  | Optional | number  | CHAP group ID for discovery session (default: 0)

Parameters `disable_chap` and `require_chap` are mutually exclusive.

### Example

Example request:

~~~
request:
{
  "params": {
    "chap_group": 1,
    "require_chap": true,
    "mutual_chap": true
  },
  "jsonrpc": "2.0",
  "method": "set_iscsi_discovery_auth",
  "id": 1
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

## add_iscsi_auth_group method {#rpc_add_iscsi_auth_group}

Add an authentication group for CHAP authentication.

### Parameters

Name                        | Optional | Type    | Description
--------------------------- | -------- | --------| -----------
tag                         | Required | number  | Authentication group tag (unique, integer > 0)
secrets                     | Optional | array   | Array of @ref rpc_add_iscsi_auth_group_secret objects

### secret {#rpc_add_iscsi_auth_group_secret}

Name                        | Optional | Type    | Description
--------------------------- | ---------| --------| -----------
user                        | Required | string  | Unidirectional CHAP name
secret                      | Required | string  | Unidirectional CHAP secret
muser                       | Optional | string  | Bidirectional CHAP name
msecret                     | Optional | string  | Bidirectional CHAP secret

### Example

Example request:

~~~
{
  "params": {
    "secrets": [
      {
        "muser": "mu1",
        "secret": "s1",
        "user": "u1",
        "msecret": "ms1"
      }
    ],
    "tag": 2
  },
  "jsonrpc": "2.0",
  "method": "add_iscsi_auth_group",
  "id": 1
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

## delete_iscsi_auth_group method {#rpc_delete_iscsi_auth_group}

Delete an existing authentication group for CHAP authentication.

### Parameters

Name                        | Optional | Type    | Description
--------------------------- | -------- | --------| -----------
tag                         | Required | number  | Authentication group tag (unique, integer > 0)

### Example

Example request:

~~~
{
  "params": {
    "tag": 2
  },
  "jsonrpc": "2.0",
  "method": "delete_iscsi_auth_group",
  "id": 1
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

## get_iscsi_auth_groups {#rpc_get_iscsi_auth_groups}

Show information about all existing authentication group for CHAP authentication.

### Parameters

This method has no parameters.

### Result

Array of objects describing authentication group.

Name                        | Type    | Description
--------------------------- | --------| -----------
tag                         | number  | Authentication group tag
secrets                     | array   | Array of @ref rpc_add_iscsi_auth_group_secret objects

### Example

Example request:

~~~
{
  "jsonrpc": "2.0",
  "method": "get_iscsi_auth_groups",
  "id": 1
}
~~~
Example response:

~~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": [
    {
      "secrets": [
        {
          "muser": "mu1",
          "secret": "s1",
          "user": "u1",
          "msecret": "ms1"
        }
      ],
      "tag": 1
    },
    {
      "secrets": [
        {
          "secret": "s2",
          "user": "u2"
        }
      ],
      "tag": 2
    }
  ]
}
~~~

## add_secret_to_iscsi_auth_group {#rpc_add_secret_to_iscsi_auth_group}

Add a secret to an existing authentication group for CHAP authentication.

### Parameters

Name                        | Optional | Type    | Description
--------------------------- | -------- | --------| -----------
tag                         | Required | number  | Authentication group tag (unique, integer > 0)
user                        | Required | string  | Unidirectional CHAP name
secret                      | Required | string  | Unidirectional CHAP secret
muser                       | Optional | string  | Bidirectional CHAP name
msecret                     | Optional | string  | Bidirectional CHAP secret

### Example

Example request:

~~~
{
  "params": {
    "muser": "mu3",
    "secret": "s3",
    "tag": 2,
    "user": "u3",
    "msecret": "ms3"
  },
  "jsonrpc": "2.0",
  "method": "add_secret_to_iscsi_auth_group",
  "id": 1
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

## delete_secret_from_iscsi_auth_group {#rpc_delete_secret_from_iscsi_auth_group}

Delete a secret from an existing authentication group for CHAP authentication.

### Parameters

Name                        | Optional | Type    | Description
--------------------------- | -------- | --------| -----------
tag                         | Required | number  | Authentication group tag (unique, integer > 0)
user                        | Required | string  | Unidirectional CHAP name

### Example

Example request:

~~~
{
  "params": {
    "tag": 2,
    "user": "u3"
  },
  "jsonrpc": "2.0",
  "method": "delete_secret_from_iscsi_auth_group",
  "id": 1
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

## get_initiator_groups method {#rpc_get_initiator_groups}

Show information about all available initiator groups.

### Parameters

This method has no parameters.

### Result

Array of objects describing initiator groups.

Name                        | Type    | Description
--------------------------- | --------| -----------
tag                         | number  | Initiator group tag
initiators                  | array   | Array of initiator hostnames or IP addresses
netmasks                    | array   | Array of initiator netmasks

### Example

Example request:

~~~
{
  "jsonrpc": "2.0",
  "method": "get_initiator_groups",
  "id": 1
}
~~~

Example response:

~~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": [
    {
      "initiators": [
        "iqn.2016-06.io.spdk:host1",
        "iqn.2016-06.io.spdk:host2"
      ],
      "tag": 1,
      "netmasks": [
        "192.168.1.0/24"
      ]
    }
  ]
}
~~~

## add_initiator_group method {#rpc_add_initiator_group}

Add an initiator group.

### Parameters

Name                        | Optional | Type    | Description
--------------------------- | -------- | --------| -----------
tag                         | Required | number  | Initiator group tag (unique, integer > 0)
initiators                  | Required | array   | Not empty array of initiator hostnames or IP addresses
netmasks                    | Required | array   | Not empty array of initiator netmasks

### Example

Example request:

~~~
{
  "params": {
    "initiators": [
      "iqn.2016-06.io.spdk:host1",
      "iqn.2016-06.io.spdk:host2"
    ],
    "tag": 1,
    "netmasks": [
      "192.168.1.0/24"
    ]
  },
  "jsonrpc": "2.0",
  "method": "add_initiator_group",
  "id": 1
}
~~~

Example response:

~~~
response:
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

## delete_initiator_group method {#rpc_delete_initiator_group}

Delete an existing initiator group.

### Parameters

Name                        | Optional | Type    | Description
--------------------------- | -------- | --------| -----------
tag                         | Required | number  | Initiator group tag (unique, integer > 0)

### Example

Example request:

~~~
{
  "params": {
    "tag": 1
  },
  "jsonrpc": "2.0",
  "method": "delete_initiator_group",
  "id": 1
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

## add_initiators_to_initiator_group method {#rpc_add_initiators_to_initiator_group}

Add initiators to an existing initiator group.

### Parameters

Name                        | Optional | Type    | Description
--------------------------- | -------- | --------| -----------
tag                         | Required | number  | Existing initiator group tag.
initiators                  | Optional | array   | Array of initiator hostnames or IP addresses
netmasks                    | Optional | array   | Array of initiator netmasks

### Example

Example request:

~~~
request:
{
  "params": {
    "initiators": [
      "iqn.2016-06.io.spdk:host3"
    ],
    "tag": 1,
    "netmasks": [
      "255.255.255.1"
    ]
  },
  "jsonrpc": "2.0",
  "method": "add_initiators_to_initiator_group",
  "id": 1
}
~~~

Example response:

~~~
response:
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

## get_target_nodes method {#rpc_get_target_nodes}

Show information about all available iSCSI target nodes.

### Parameters

This method has no parameters.

### Result

Array of objects describing target node.

Name                        | Type    | Description
--------------------------- | --------| -----------
name                        | string  | Target node name (ASCII)
alias_name                  | string  | Target node alias name (ASCII)
pg_ig_maps                  | array   | Array of Portal_Group_Tag:Initiator_Group_Tag mappings
luns                        | array   | Array of Bdev names to LUN ID mappings
queue_depth                 | number  | Target queue depth
disable_chap                | boolean | CHAP authentication should be disabled for this target
require_chap                | boolean | CHAP authentication should be required for this target
mutual_chap                 | boolean | CHAP authentication should be bidirectional (`true`) or unidirectional (`false`)
chap_group                  | number  | Authentication group ID for this target node
header_digest               | boolean | Header Digest should be required for this target node
data_digest                 | boolean | Data Digest should be required for this target node

### Example

Example request:

~~~
{
  "jsonrpc": "2.0",
  "method": "get_target_nodes",
  "id": 1
}
~~~

Example response:

~~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": [
    {
      "luns": [
        {
          "lun_id": 0,
          "bdev_name": "Nvme0n1"
        }
      ],
      "mutual_chap": false,
      "name": "iqn.2016-06.io.spdk:target1",
      "alias_name": "iscsi-target1-alias",
      "require_chap": false,
      "chap_group": 0,
      "pg_ig_maps": [
        {
          "ig_tag": 1,
          "pg_tag": 1
        }
      ],
      "data_digest": false,
      "disable_chap": false,
      "header_digest": false,
      "queue_depth": 64
    }
  ]
}
~~~

## construct_target_node method {#rpc_construct_target_node}

Add a iSCSI target node.

### Parameters

Name                        | Optional | Type    | Description
--------------------------- | -------- | --------| -----------
name                        | Required | string  | Target node name (ASCII)
alias_name                  | Required | string  | Target node alias name (ASCII)
pg_ig_maps                  | Required | array   | Array of (Portal_Group_Tag:Initiator_Group_Tag) mappings
luns                        | Required | array   | Array of Bdev names to LUN ID mappings
queue_depth                 | Required | number  | Target queue depth
disable_chap                | Optional | boolean | CHAP authentication should be disabled for this target
require_chap                | Optional | boolean | CHAP authentication should be required for this target
mutual_chap                 | Optional | boolean | CHAP authentication should be bidirectional (`true`) or unidirectional (`false`)
chap_group                  | Optional | number  | Authentication group ID for this target node
header_digest               | Optional | boolean | Header Digest should be required for this target node
data_digest                 | Optional | boolean | Data Digest should be required for this target node

Parameters `disable_chap` and `require_chap` are mutually exclusive.

### Example

Example request:

~~~
{
  "params": {
    "luns": [
      {
        "lun_id": 0,
        "bdev_name": "Nvme0n1"
      }
    ],
    "mutual_chap": true,
    "name": "target2",
    "alias_name": "iscsi-target2-alias",
    "pg_ig_maps": [
      {
        "ig_tag": 1,
        "pg_tag": 1
      },
      {
        "ig_tag": 2,
        "pg_tag": 2
      }
    ],
    "data_digest": true,
    "disable_chap": true,
    "header_digest": true,
    "queue_depth": 24
  },
  "jsonrpc": "2.0",
  "method": "construct_target_node",
  "id": 1
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

## set_iscsi_target_node_auth method {#rpc_set_iscsi_target_node_auth}

Set CHAP authentication to an existing iSCSI target node.

### Parameters

Name                        | Optional | Type    | Description
--------------------------- | -------- | --------| -----------
name                        | Required | string  | Target node name (ASCII)
disable_chap                | Optional | boolean | CHAP authentication should be disabled for this target
require_chap                | Optional | boolean | CHAP authentication should be required for this target
mutual_chap                 | Optional | boolean | CHAP authentication should be bidirectional (`true`) or unidirectional (`false`)
chap_group                  | Optional | number  | Authentication group ID for this target node

Parameters `disable_chap` and `require_chap` are mutually exclusive.

### Example

Example request:

~~~
{
  "params": {
    "chap_group": 1,
    "require_chap": true,
    "name": "iqn.2016-06.io.spdk:target1",
    "mutual_chap": true
  },
  "jsonrpc": "2.0",
  "method": "set_iscsi_target_node_auth",
  "id": 1
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

## add_pg_ig_maps method {#rpc_add_pg_ig_maps}

Add initiator group to portal group mappings to an existing iSCSI target node.

### Parameters

Name                        | Optional | Type    | Description
--------------------------- | -------- | --------| -----------
name                        | Required | string  | Target node name (ASCII)
pg_ig_maps                  | Required | array   | Not empty array of initiator to portal group mappings objects

Portal to Initiator group mappings object:

Name                        | Optional | Type    | Description
--------------------------- | -------- | --------| -----------
ig_tag                      | Required | number  | Existing initiator group tag
pg_tag                      | Required | number  | Existing portal group tag

### Example

Example request:

~~~
{
  "params": {
    "pg_ig_maps": [
      {
        "ig_tag": 1,
        "pg_tag": 1
      },
      {
        "ig_tag": 2,
        "pg_tag": 2
      },
      {
        "ig_tag": 3,
        "pg_tag": 3
      }
    ],
    "name": "iqn.2016-06.io.spdk:target3"
  },
  "jsonrpc": "2.0",
  "method": "add_pg_ig_maps",
  "id": 1
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

## delete_pg_ig_maps method {#rpc_delete_pg_ig_maps}

Delete initiator group to portal group mappings from an existing iSCSI target node.

### Parameters

Name                        | Optional | Type    | Description
--------------------------- | -------- | --------| -----------
name                        | Required | string  | Target node name (ASCII)
pg_ig_maps                  | Required | array   | Not empty array of Portal to Initiator group mappings objects

Portal to Initiator group mappings object:

Name                        | Optional | Type    | Description
--------------------------- | -------- | --------| -----------
ig_tag                      | Required | number  | Existing initiator group tag
pg_tag                      | Required | number  | Existing portal group tag

### Example

Example request:

~~~
{
  "params": {
    "pg_ig_maps": [
      {
        "ig_tag": 1,
        "pg_tag": 1
      },
      {
        "ig_tag": 2,
        "pg_tag": 2
      },
      {
        "ig_tag": 3,
        "pg_tag": 3
      }
    ],
    "name": "iqn.2016-06.io.spdk:target3"
  },
  "jsonrpc": "2.0",
  "method": "delete_pg_ig_maps",
  "id": 1
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

## delete_target_node method {#rpc_delete_target_node}

Delete a iSCSI target node.

### Parameters

Name                        | Optional | Type    | Description
--------------------------- | -------- | --------| -----------
name                        | Required | string  | Target node name (ASCII)

### Example

Example request:

~~~
{
  "params": {
    "name": "iqn.2016-06.io.spdk:target1"
  },
  "jsonrpc": "2.0",
  "method": "delete_target_node",
  "id": 1
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

## get_portal_groups method {#rpc_get_portal_groups}

Show information about all available portal groups.

### Parameters

This method has no parameters.

### Example

Example request:

~~~
request:
{
  "jsonrpc": "2.0",
  "method": "get_portal_groups",
  "id": 1
}
~~~

Example response:

~~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": [
    {
      "portals": [
        {
          "cpumask": "0x2",
          "host": "127.0.0.1",
          "port": "3260"
        }
      ],
      "tag": 1
    }
  ]
}
~~~

## add_portal_group method {#rpc_add_portal_group}

Add a portal group.

### Parameters

Name                        | Optional | Type    | Description
--------------------------- | -------- | --------| -----------
tag                         | Required | number  | Portal group tag
portals                     | Required | array   | Not empty array of portals

Portal object

Name                        | Optional | Type    | Description
--------------------------- | -------- | --------| -----------
host                        | Required | string  | Hostname or IP address
port                        | Required | string  | Port number

### Example

Example request:

~~~
{
  "params": {
    "portals": [
      {
        "host": "127.0.0.1",
        "port": "3260"
      }
    ],
    "tag": 1
  },
  "jsonrpc": "2.0",
  "method": "add_portal_group",
  "id": 1
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

## delete_portal_group method {#rpc_delete_portal_group}

Delete an existing portal group.

### Parameters

Name                        | Optional | Type    | Description
--------------------------- | -------- | --------| -----------
tag                         | Required | number  | Existing portal group tag

### Example

Example request:

~~~
{
  "params": {
    "tag": 1
  },
  "jsonrpc": "2.0",
  "method": "delete_portal_group",
  "id": 1
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

## get_iscsi_connections method {#rpc_get_iscsi_connections}

Show information about all active connections.

### Parameters

This method has no parameters.

### Results

Array of objects describing iSCSI connection.

Name                        | Type    | Description
--------------------------- | --------| -----------
id                          | number  | Index (used for TTT - Target Transfer Tag)
cid                         | number  | CID (Connection ID)
tsih                        | number  | TSIH (Target Session Identifying Handle)
lcore_id                    | number  | Core number on which the iSCSI connection runs
initiator_addr              | string  | Initiator address
target_addr                 | string  | Target address
target_node_name            | string  | Target node name (ASCII) without prefix

### Example

Example request:

~~~
{
  "jsonrpc": "2.0",
  "method": "get_iscsi_connections",
  "id": 1
}
~~~

Example response:

~~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": [
    {
      "tsih": 4,
      "cid": 0,
      "target_node_name": "target1",
      "lcore_id": 0,
      "initiator_addr": "10.0.0.2",
      "target_addr": "10.0.0.1",
      "id": 0
    }
  ]
}
~~~

## target_node_add_lun method {#rpc_target_node_add_lun}

Add an LUN to an existing iSCSI target node.

### Parameters

Name                        | Optional | Type    | Description
--------------------------- | -------- | --------| -----------
name                        | Required | string  | Target node name (ASCII)
bdev_name                   | Required | string  | bdev name to be added as a LUN
lun_id                      | Optional | number  | LUN ID (default: first free ID)

### Example

Example request:

~~~
{
  "params": {
    "lun_id": 2,
    "name": "iqn.2016-06.io.spdk:target1",
    "bdev_name": "Malloc0"
  },
  "jsonrpc": "2.0",
  "method": "target_node_add_lun",
  "id": 1
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

## nvmf_subsystem_create method {#rpc_nvmf_subsystem_create}

Construct an NVMe over Fabrics target subsystem.

### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
nqn                     | Required | string      | Subsystem NQN
serial_number           | Optional | string      | Serial number of virtual controller
max_namespaces          | Optional | number      | Maximum number of namespaces that can be attached to the subsystem. Default: 0 (Unlimited)
allow_any_host          | Optional | boolean     | Allow any host (`true`) or enforce allowed host whitelist (`false`). Default: `false`.

### Example

Example request:

~~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "nvmf_subsystem_create",
  "params": {
    "nqn": "nqn.2016-06.io.spdk:cnode1",
    "allow_any_host": false,
    "serial_number": "abcdef",
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
listen_address          | Required | object      | @ref rpc_nvmf_listen_address object

### listen_address {#rpc_nvmf_listen_address}

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
trtype                  | Required | string      | Transport type ("RDMA")
adrfam                  | Required | string      | Address family ("IPv4", "IPv6", "IB", or "FC")
traddr                  | Required | string      | Transport address
trsvcid                 | Required | string      | Transport service ID

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
namespace               | Required | object      | @ref rpc_nvmf_namespace object

### namespace {#rpc_nvmf_namespace}

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

Set global parameters for the NVMe-oF target.  This RPC may only be called before SPDK subsystems
have been initialized.

### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
max_queue_depth         | Optional | number      | Maximum number of outstanding I/Os per queue
max_qpairs_per_ctrlr    | Optional | number      | Maximum number of SQ and CQ per controller
in_capsule_data_size    | Optional | number      | Maximum number of in-capsule data size
max_io_size             | Optional | number      | Maximum I/O size (bytes)
max_subsystems          | Optional | number      | Maximum number of NVMe-oF subsystems
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

Set global configuration of NVMe-oF target.  This RPC may only be called before SPDK subsystems
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

# Vhost Target {#jsonrpc_components_vhost_tgt}

The following common preconditions need to be met in all target types.

Controller name will be used to create UNIX domain socket. This implies that name concatenated with vhost socket
directory path needs to be valid UNIX socket name.

@ref cpu_mask parameter is used to choose CPU on which pollers will be launched when new initiator is connecting.
It must be a subset of application CPU mask. Default value is CPU mask of the application.

## set_vhost_controller_coalescing {#rpc_set_vhost_controller_coalescing}

Controls interrupt coalescing for specific target. Because `delay_base_us` is used to calculate delay in CPU ticks
there is no hardcoded limit for this parameter. Only limitation is that final delay in CPU ticks might not overflow
32 bit unsigned integer (which is more than 1s @ 4GHz CPU). In real scenarios `delay_base_us` should be much lower
than 150us. To disable coalescing set `delay_base_us` to 0.

### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
ctrlr                   | Required | string      | Controller name
delay_base_us           | Required | number      | Base (minimum) coalescing time in microseconds
iops_threshold          | Required | number      | Coalescing activation level greater than 0 in IO per second

### Example

Example request:

~~~
{
  "params": {
    "iops_threshold": 100000,
    "ctrlr": "VhostScsi0",
    "delay_base_us": 80
  },
  "jsonrpc": "2.0",
  "method": "set_vhost_controller_coalescing",
  "id": 1
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

## construct_vhost_scsi_controller {#rpc_construct_vhost_scsi_controller}

Construct vhost SCSI target.

### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
ctrlr                   | Required | string      | Controller name
cpumask                 | Optional | string      | @ref cpu_mask for this controller

### Example

Example request:

~~~
{
  "params": {
    "cpumask": "0x2",
    "ctrlr": "VhostScsi0"
  },
  "jsonrpc": "2.0",
  "method": "construct_vhost_scsi_controller",
  "id": 1
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

## add_vhost_scsi_lun {#rpc_add_vhost_scsi_lun}

In vhost target `ctrlr` create SCSI target with ID `scsi_target_num` and add `bdev_name` as LUN 0.

### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
ctrlr                   | Required | string      | Controller name
scsi_target_num         | Required | number      | SCSI target ID between 0 and 7
bdev_name               | Required | string      | Name of bdev to expose as a LUN 0

### Example

Example request:

~~~
{
  "params": {
    "scsi_target_num": 1,
    "bdev_name": "Malloc0",
    "ctrlr": "VhostScsi0"
  },
  "jsonrpc": "2.0",
  "method": "add_vhost_scsi_lun",
  "id": 1
}
~~~

Example response:

~~~
response:
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

## remove_vhost_scsi_target {#rpc_remove_vhost_scsi_target}

Remove SCSI target ID `scsi_target_num` from vhost target `scsi_target_num`.

This method will fail if initiator is connected, but doesn't support hot-remove (the `VIRTIO_SCSI_F_HOTPLUG` is not negotiated).

### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
ctrlr                   | Required | string      | Controller name
scsi_target_num         | Required | number      | SCSI target ID between 0 and 7

### Example

Example request:

~~~
request:
{
  "params": {
    "scsi_target_num": 1,
    "ctrlr": "VhostScsi0"
  },
  "jsonrpc": "2.0",
  "method": "remove_vhost_scsi_target",
  "id": 1
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

## construct_vhost_nvme_controller {#rpc_construct_vhost_nvme_controller}

Construct empty vhost NVMe controller.

### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
ctrlr                   | Required | string      | Controller name
io_queues               | Required | number      | Number between 1 and 31 of IO queues for the controller
cpumask                 | Optional | string      | @ref cpu_mask for this controller


### Example

Example request:

~~~
{
  "params": {
    "cpumask": "0x2",
    "io_queues": 4,
    "ctrlr": "VhostNvme0"
  },
  "jsonrpc": "2.0",
  "method": "construct_vhost_nvme_controller",
  "id": 1
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

## add_vhost_nvme_ns {#rpc_add_vhost_nvme_ns}

Add namespace backed by `bdev_name`

### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
ctrlr                   | Required | string      | Controller name
bdev_name               | Required | string      | Name of bdev to expose as a namespace
cpumask                 | Optional | string      | @ref cpu_mask for this controller

### Example

Example request:

~~~
{
  "params": {
    "bdev_name": "Malloc0",
    "ctrlr": "VhostNvme0"
  },
  "jsonrpc": "2.0",
  "method": "add_vhost_nvme_ns",
  "id": 1
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

## construct_vhost_blk_controller {#rpc_construct_vhost_blk_controller}

Construct vhost block controller

If `readonly` is `true` then vhost block target will be created as read only and fail any write requests.
The `VIRTIO_BLK_F_RO` feature flag will be offered to the initiator.

### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
ctrlr                   | Required | string      | Controller name
bdev_name               | Required | string      | Name of bdev to expose block device
readonly                | Optional | boolean     | If true, this target will be read only (default: false)
cpumask                 | Optional | string      | @ref cpu_mask for this controller


### Example

Example request:

~~~
{
  "params": {
    "dev_name": "Malloc0",
    "ctrlr": "VhostBlk0"
  },
  "jsonrpc": "2.0",
  "method": "construct_vhost_blk_controller",
  "id": 1
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

## get_vhost_controllers {#rpc_get_vhost_controllers}

Display information about all or specific vhost controller(s).

### Parameters

The user may specify no parameters in order to list all controllers, or a controller may be
specified by name.

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Optional | string      | Vhost controller name


### Response {#rpc_get_vhost_controllers_response}

Response is an array of objects describing requested controller(s). Common fields are:

Name                    | Type        | Description
----------------------- | ----------- | -----------
ctrlr                   | string      | Controller name
cpumask                 | string      | @ref cpu_mask of this controller
delay_base_us           | number      | Base (minimum) coalescing time in microseconds (0 if disabled)
iops_threshold          | number      | Coalescing activation level
backend_specific        | object      | Backend specific informations

### Vhost block {#rpc_get_vhost_controllers_blk}

`backend_specific` contains one `block` object  of type:

Name                    | Type        | Description
----------------------- | ----------- | -----------
bdev                    | string      | Backing bdev name or Null if bdev is hot-removed
readonly                | boolean     | True if controllers is readonly, false otherwise

### Vhost SCSI {#rpc_get_vhost_controllers_scsi}

`backend_specific` contains `scsi` array of following objects:

Name                    | Type        | Description
----------------------- | ----------- | -----------
target_name             | string      | Name of this SCSI target
id                      | number      | Unique SPDK global SCSI target ID
scsi_dev_num            | number      | SCSI target ID initiator will see when scanning this controller
luns                    | array       | array of objects describing @ref rpc_get_vhost_controllers_scsi_luns

### Vhost SCSI LUN {#rpc_get_vhost_controllers_scsi_luns}

Object of type:

Name                    | Type        | Description
----------------------- | ----------- | -----------
id                      | number      | SCSI LUN ID
bdev_name               | string      | Backing bdev name

### Vhost NVMe {#rpc_get_vhost_controllers_nvme}

`backend_specific` contains `namespaces` array of following objects:

Name                    | Type        | Description
----------------------- | ----------- | -----------
nsid                    | number      | Namespace ID
bdev                    | string      | Backing bdev name

### Example

Example request:

~~~
{
  "jsonrpc": "2.0",
  "method": "get_vhost_controllers",
  "id": 1
}
~~~

Example response:

~~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": [
    {
      "cpumask": "0x2",
      "backend_specific": {
        "block": {
          "readonly": false,
          "bdev": "Malloc0"
        }
      },
      "iops_threshold": 60000,
      "ctrlr": "VhostBlk0",
      "delay_base_us": 100
    },
    {
      "cpumask": "0x2",
      "backend_specific": {
        "scsi": [
          {
            "target_name": "Target 2",
            "luns": [
              {
                "id": 0,
                "bdev_name": "Malloc1"
              }
            ],
            "id": 0,
            "scsi_dev_num": 2
          },
          {
            "target_name": "Target 5",
            "luns": [
              {
                "id": 0,
                "bdev_name": "Malloc2"
              }
            ],
            "id": 1,
            "scsi_dev_num": 5
          }
        ]
      },
      "iops_threshold": 60000,
      "ctrlr": "VhostScsi0",
      "delay_base_us": 0
    },
    {
      "cpumask": "0x2",
      "backend_specific": {
        "namespaces": [
          {
            "bdev": "Malloc3",
            "nsid": 1
          },
          {
            "bdev": "Malloc4",
            "nsid": 2
          }
        ]
      },
      "iops_threshold": 60000,
      "ctrlr": "VhostNvme0",
      "delay_base_us": 0
    }
  ]
}
~~~

## remove_vhost_controller {#rpc_remove_vhost_controller}

Remove vhost target.

This call will fail if there is an initiator connected or there is at least one SCSI target configured in case of
vhost SCSI target. In the later case please remove all SCSI targets first using @ref rpc_remove_vhost_scsi_target.

### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
ctrlr                   | Required | string      | Controller name

### Example

Example request:

~~~
{
  "params": {
    "ctrlr": "VhostNvme0"
  },
  "jsonrpc": "2.0",
  "method": "remove_vhost_controller",
  "id": 1
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

# Logical Volume {#jsonrpc_components_lvol}

Identification of logical volume store and logical volume is explained first.

A logical volume store has a UUID and a name for its identification.
The UUID is generated on creation and it can be used as a unique identifier.
The name is specified on creation and can be renamed.
Either UUID or name is used to access logical volume store in RPCs.

A logical volume has a UUID and a name for its identification.
The UUID of the logical volume is generated on creation and it can be unique identifier.
The alias of the logical volume takes the format _lvs_name/lvol_name_ where:
* _lvs_name_ is the name of the logical volume store.
* _lvol_name_ is specified on creation and can be renamed.

## construct_lvol_store {#rpc_construct_lvol_store}

Construct a logical volume store.

### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
bdev_name               | Required | string      | Bdev on which to construct logical volume store
lvs_name                | Required | string      | Name of the logical volume store to create
cluster_sz              | Optional | number      | Cluster size of the logical volume store in bytes

### Response

UUID of the created logical volume store is returned.

### Example

Example request:
~~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "construct_lvol_store",
  "params": {
    "lvs_name": "LVS0",
    "bdev_name": "Malloc0"
  }
}
~~~

Example response:
~~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": "a9959197-b5e2-4f2d-8095-251ffb6985a5"
}
~~~

## destroy_lvol_store {#rpc_destroy_lvol_store}

Destroy a logical volume store.

### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
uuid                    | Optional | string      | UUID of the logical volume store to destroy
lvs_name                | Optional | string      | Name of the logical volume store to destroy

Either uuid or lvs_name must be specified, but not both.

### Example

Example request:
~~~
{
  "jsonrpc": "2.0",
  "method": "destroy_lvol_store",
  "id": 1
  "params": {
    "uuid": "a9959197-b5e2-4f2d-8095-251ffb6985a5"
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

## get_lvol_stores {#rpc_get_lvol_stores}

Get a list of logical volume stores.

### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
uuid                    | Optional | string      | UUID of the logical volume store to retrieve information about
lvs_name                | Optional | string      | Name of the logical volume store to retrieve information about

Either uuid or lvs_name may be specified, but not both.
If both uuid and lvs_name are omitted, information about all logical volume stores is returned.

### Example

Example request:
~~~
{
  "jsonrpc": "2.0",
  "method": "get_lvol_stores",
  "id": 1,
  "params": {
    "lvs_name": "LVS0"
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
      "uuid": "a9959197-b5e2-4f2d-8095-251ffb6985a5",
      "base_bdev": "Malloc0",
      "free_clusters": 31,
      "cluster_size": 4194304,
      "total_data_clusters": 31,
      "block_size": 4096,
      "name": "LVS0"
    }
  ]
}
~~~

## rename_lvol_store {#rpc_rename_lvol_store}

Rename a logical volume store.

### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
old_name                | Required | string      | Existing logical volume store name
new_name                | Required | string      | New logical volume store name

### Example

Example request:
~~~
{
  "jsonrpc": "2.0",
  "method": "rename_lvol_store",
  "id": 1,
  "params": {
    "old_name": "LVS0",
    "new_name": "LVS2"
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

## construct_lvol_bdev {#rpc_construct_lvol_bdev}

Create a logical volume on a logical volume store.

### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
lvol_name               | Required | string      | Name of logical volume to create
size                    | Required | number      | Desired size of logical volume in bytes
thin_provision          | Optional | boolean     | True to enable thin provisioning
uuid                    | Optional | string      | UUID of logical volume store to create logical volume on
lvs_name                | Optional | string      | Name of logical volume store to create logical volume on

Size will be rounded up to a multiple of cluster size. Either uuid or lvs_name must be specified, but not both.
lvol_name will be used in the alias of the created logical volume.

### Response

UUID of the created logical volume is returned.

### Example

Example request:
~~~
{
  "jsonrpc": "2.0",
  "method": "construct_lvol_bdev",
  "id": 1,
  "params": {
    "lvol_name": "LVOL0",
    "size": 1048576,
    "lvs_name": "LVS0",
    "thin_provision": true
  }
}
~~~

Example response:
~~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": "1b38702c-7f0c-411e-a962-92c6a5a8a602"
}
~~~

## snapshot_lvol_bdev {#rpc_snapshot_lvol_bdev}

Capture a snapshot of the current state of a logical volume.

### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
lvol_name               | Required | string      | UUID or alias of the logical volume to create a snapshot from
snapshot_name           | Required | string      | Name for the newly created snapshot

### Response

UUID of the created logical volume snapshot is returned.

### Example

Example request:
~~~
{
  "jsonrpc": "2.0",
  "method": "snapshot_lvol_bdev",
  "id": 1,
  "params": {
    "lvol_name": "1b38702c-7f0c-411e-a962-92c6a5a8a602",
    "snapshot_name": "SNAP1"
  }
}
~~~

Example response:
~~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": "cc8d7fdf-7865-4d1f-9fc6-35da8e368670"
}
~~~

## clone_lvol_bdev {#rpc_clone_lvol_bdev}

Create a logical volume based on a snapshot.

### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
snapshot_name           | Required | string      | UUID or alias of the snapshot to clone
clone_name              | Required | string      | Name for the logical volume to create

### Response

UUID of the created logical volume clone is returned.

### Example

Example request:
~~~
{
  "jsonrpc": "2.0"
  "method": "clone_lvol_bdev",
  "id": 1,
  "params": {
    "snapshot_name": "cc8d7fdf-7865-4d1f-9fc6-35da8e368670",
    "clone_name": "CLONE1"
  }
}
~~~

Example response:
~~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": "8d87fccc-c278-49f0-9d4c-6237951aca09"
}
~~~

## rename_lvol_bdev {#rpc_rename_lvol_bdev}

Rename a logical volume. New name will rename only the alias of the logical volume.

### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
old_name                | Required | string      | UUID or alias of the existing logical volume
new_name                | Required | string      | New logical volume name

### Example

Example request:
~~~
{
  "jsonrpc": "2.0",
  "method": "rename_lvol_bdev",
  "id": 1,
  "params": {
    "old_name": "067df606-6dbc-4143-a499-0d05855cb3b8",
    "new_name": "LVOL2"
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

## resize_lvol_bdev {#rpc_resize_lvol_bdev}

Resize a logical volume.

### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | UUID or alias of the logical volume to resize
size                    | Required | number      | Desired size of the logical volume in bytes

### Example

Example request:
~~~
{
  "jsonrpc": "2.0",
  "method": "resize_lvol_bdev",
  "id": 1,
  "params": {
    "name": "51638754-ca16-43a7-9f8f-294a0805ab0a",
    "size": 2097152
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

## destroy_lvol_bdev {#rpc_destroy_lvol_bdev}

Destroy a logical volume.

### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | UUID or alias of the logical volume to destroy

### Example

Example request:
~~~
{
  "jsonrpc": "2.0",
  "method": "destroy_lvol_bdev",
  "id": 1,
  "params": {
    "name": "51638754-ca16-43a7-9f8f-294a0805ab0a"
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

## inflate_lvol_bdev {#rpc_inflate_lvol_bdev}

Inflate a logical volume. All unallocated clusters are allocated and copied from the parent or zero filled if not allocated in the parent. Then all dependencies on the parent are removed.

### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | UUID or alias of the logical volume to inflate

### Example

Example request:
~~~
{
  "jsonrpc": "2.0",
  "method": "inflate_lvol_bdev",
  "id": 1,
  "params": {
    "name": "8d87fccc-c278-49f0-9d4c-6237951aca09"
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

## decouple_parent_lvol_bdev {#rpc_decouple_parent_lvol_bdev}

Decouple the parent of a logical volume. For unallocated clusters which is allocated in the parent, they are allocated and copied from the parent, but for unallocated clusters which is thin provisioned in the parent, they are kept thin provisioned. Then all dependencies on the parent are removed.

### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | UUID or alias of the logical volume to decouple the parent of it

### Example

Example request:
~~~
{
  "jsonrpc": "2.0",
  "method": "decouple_parent_lvol_bdev",
  "id": 1.
  "params": {
    "name": "8d87fccc-c278-49f0-9d4c-6237951aca09"
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

## send_nvme_cmd {#rpc_send_nvme_cmd}

Send NVMe command directly to NVMe controller or namespace. Parameters and responses encoded by base64 urlsafe need further processing.

Notice: send_nvme_cmd requires user to guarentee the correctness of NVMe command itself, and also optional parameters. Illegal command contents or mismatching buffer size may result in unpredictable behavior.

### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | Name of the operating NVMe controller
cmd_type                | Required | string      | Type of nvme cmd. Valid values are: admin, io
data_direction          | Required | string      | Direction of data transfer. Valid values are: c2h, h2c
cmdbuf                  | Required | string      | NVMe command encoded by base64 urlsafe
data                    | Optional | string      | Data transferring to controller from host, encoded by base64 urlsafe
metadata                | Optional | string      | Metadata transferring to controller from host, encoded by base64 urlsafe
data_len                | Optional | number      | Data length required to transfer from controller to host
metadata_len            | Optional | number      | Metadata length required to transfer from controller to host
timeout_ms              | Optional | number      | Command execution timeout value, in milliseconds

### Response

Name                    | Type        | Description
----------------------- | ----------- | -----------
cpl                     | string      | NVMe completion queue entry, encoded by base64 urlsafe
data                    | string      | Data transferred from controller to host, encoded by base64 urlsafe
metadata                | string      | Metadata transferred from controller to host, encoded by base64 urlsafe

### Example

Example request:
~~~
{
  "jsonrpc": "2.0",
  "method": "send_nvme_cmd",
  "id": 1,
  "params": {
    "name": "Nvme0",
    "cmd_type": "admin"
    "data_direction": "c2h",
    "cmdbuf": "BgAAAAEAAAAAAAAAAAAAAAAAAAAAAAAAsGUs9P5_AAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA==",
    "data_len": 60,
  }
}
~~~

Example response:
~~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "result":  {
    "cpl": "AAAAAAAAAAARAAAAWrmwABAA==",
    "data": "sIjg6AAAAACwiODoAAAAALCI4OgAAAAAAAYAAREAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
  }

}
~~~
