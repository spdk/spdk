# JSON-RPC {#jsonrpc}

## Overview {#jsonrpc_overview}

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
-1     | Invalid state - given method exists but it is not callable in [current runtime state](@ref rpc_framework_start_init)
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

## rpc.py {#rpc_py}

SPDK provides a set of Python scripts which can invoke the JSON-RPC methods described in this document.  'rpc.py' in the scripts
directory is the main script that users will invoke to execute a JSON-RPC method. The scripts/rpc directory contains modules
that 'rpc.py' imports for definitions of each SPDK library's or module's methods.

Example:

~~~bash
scripts/rpc.py bdev_nvme_attach_controller -b nvme0 -a 00:02.0 -t pcie
~~~

A brief description of each of the RPC methods and optional 'rpc.py' arguments can be viewed with:

~~~bash
scripts/rpc.py --help
~~~

A detailed description of each RPC method and its parameters is also available.  For example:

~~~bash
scripts/rpc.py bdev_nvme_attach_controller --help
~~~

### Generate JSON-RPC methods for current configuration {#jsonrpc_generate}

An initial configuration can be specified for an SPDK application via the '-c' command line parameter.
The configuration file is a JSON file containing all of the JSON-RPC method invocations necessary
for the desired configuration. Writing these files by hand is extremely tedious however, so 'rpc.py'
provides a mechanism to generate a JSON-RPC file based on the current configuration.

~~~bash
scripts/rpc.py save_config > config.json
~~~

'config.json' can then be passed to a new SPDK application using the '-c' command line parameter
to reproduce the same configuration.

### JSON-RPC batching

'rpc.py' also supports batching of multiple JSON-RPC methods with one invocation.  So instead of
calling 'rpc.py' once for each JSON-RPC method, such as:

~~~bash
scripts/rpc.py bdev_malloc_create -b malloc0 64 512
scripts/rpc.py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a
scripts/rpc.py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 malloc0
scripts/rpc.py nvmf_create_transport -t tcp
scripts/rpc.py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t tcp -a 127.0.0.1 -s 4420
~~~

you can put the following into a text file:

~~~bash
bdev_malloc_create -b malloc0 64 512
nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a
nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 malloc0
nvmf_create_transport -t tcp
nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t tcp -a 127.0.0.1 -s 4420
~~~

and then just do:

~~~bash
scripts/rpc.py < rpc.txt
~~~

### Adding external RPC methods

SPDK includes both in-tree modules as well as the ability to use external modules.  The in-tree modules include some python
scripts to ease the process of sending RPCs to in-tree modules.  External modules can utilize this same framework to add new RPC
methods as follows:

If PYTHONPATH doesn't include the location of the external RPC python script, it should be updated:

~~~bash
export PYTHONPATH=/home/user/plugin_example/
~~~

In provided location, create python module file (e.g. rpc_plugin.py) with new RPC methods.  The file should contain
spdk_rpc_plugin_initialize() method that will be called when the plugin is loaded to define new parsers for provided subparsers
argument that adds new RPC calls (subparsers.add_parser()).  The new parsers should use the client.call() method to call RPC
functions registered within the external module using the SPDK_RPC_REGISTER() macro.  Example:

~~~python
from rpc.client import print_json


def example_create(client, num_blocks, block_size, name=None, uuid=None):
    """Construct an example block device.

    Args:
        num_blocks: size of block device in blocks
        block_size: block size of device; must be a power of 2 and at least 512
        name: name of block device (optional)
        uuid: UUID of block device (optional)

    Returns:
        Name of created block device.
    """
    params = {'num_blocks': num_blocks, 'block_size': block_size}
    if name:
        params['name'] = name
    if uuid:
        params['uuid'] = uuid
    return client.call('bdev_example_create', params)


def example_delete(client, name):
    """Delete example block device.

    Args:
        bdev_name: name of bdev to delete
    """
    params = {'name': name}
    return client.call('bdev_example_delete', params)


def spdk_rpc_plugin_initialize(subparsers):
    def bdev_example_create(args):
        num_blocks = (args.total_size * 1024 * 1024) // args.block_size
        print_json(example_create(args.client,
                                  num_blocks=int(num_blocks),
                                  block_size=args.block_size,
                                  name=args.name,
                                  uuid=args.uuid))

    p = subparsers.add_parser('bdev_example_create',
                              help='Create an example bdev')
    p.add_argument('-b', '--name', help="Name of the bdev")
    p.add_argument('-u', '--uuid', help="UUID of the bdev")
    p.add_argument(
        'total_size', help='Size of bdev in MB (float > 0)', type=float)
    p.add_argument('block_size', help='Block size for this bdev', type=int)
    p.set_defaults(func=bdev_example_create)

    def bdev_example_delete(args):
        example_delete(args.client,
                      name=args.name)

    p = subparsers.add_parser('bdev_example_delete',
                              help='Delete an example disk')
    p.add_argument('name', help='example bdev name')
    p.set_defaults(func=bdev_example_delete)
~~~

Finally, call the rpc.py script with '--plugin' parameter to provide above python module name:

~~~bash
./scripts/rpc.py --plugin rpc_plugin bdev_example_create 10 4096
~~~

### Converting from legacy configuration {#jsonrpc_convert}

Starting with SPDK 20.10, legacy configuration file support has been removed.
Users with extensive configuration files already running in SPDK application,
can [generate JSON-RPC for current configuration](@ref jsonrpc_generate).

If binary for deploying the application is unavailable, the legacy configuration
file can be converted to JSON-RPC using python tool:

~~~bash
./scripts/config_converter.py < config.ini > config.json
~~~

## App Framework {#jsonrpc_components_app}

### spdk_kill_instance {#rpc_spdk_kill_instance}

Send a signal to the application.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
sig_name                | Required | string      | Signal to send (SIGINT, SIGTERM, SIGQUIT, SIGHUP, or SIGKILL)

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "spdk_kill_instance",
  "params": {
    "sig_name": "SIGINT"
  }
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### framework_monitor_context_switch {#rpc_framework_monitor_context_switch}

Query, enable, or disable the context switch monitoring functionality.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
enabled                 | Optional | boolean     | Enable (`true`) or disable (`false`) monitoring (omit this parameter to query the current state)

#### Response

Name                    | Type        | Description
----------------------- | ----------- | -----------
enabled                 | boolean     | The current state of context switch monitoring

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "framework_monitor_context_switch",
  "params": {
    "enabled": false
  }
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "enabled": false
  }
}
~~~

### framework_start_init {#rpc_framework_start_init}

Start initialization of SPDK subsystems when it is deferred by starting SPDK application with option -w.
During its deferral some RPCs can be used to set global parameters for SPDK subsystems.
This RPC can be called only once.

#### Parameters

This method has no parameters.

#### Response

Completion status of SPDK subsystem initialization is returned as a boolean.

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "framework_start_init"
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### framework_wait_init {#rpc_framework_wait_init}

Do not return until all subsystems have been initialized and the RPC system state is running.
If the application is already running, this call will return immediately. This RPC can be called at any time.

#### Parameters

This method has no parameters.

#### Response

Returns True when subsystems have been initialized.

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "framework_wait_init"
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### rpc_get_methods {#rpc_rpc_get_methods}

Get an array of supported RPC methods.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
current                 | Optional | boolean     | Get an array of RPC methods only callable in the current state.

#### Response

The response is an array of supported RPC methods.

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "rpc_get_methods"
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": [
    "framework_start_init",
    "rpc_get_methods",
    "scsi_get_devices",
    "nbd_get_disks",
    "nbd_stop_disk",
    "nbd_start_disk",
    "log_get_flags",
    "log_clear_flag",
    "log_set_flag",
    "log_get_level",
    "log_set_level",
    "log_get_print_level",
    "log_set_print_level",
    "iscsi_get_options",
    "iscsi_target_node_add_lun",
    "iscsi_get_connections",
    "iscsi_delete_portal_group",
    "iscsi_create_portal_group",
    "iscsi_get_portal_groups",
    "iscsi_delete_target_node",
    "iscsi_target_node_remove_pg_ig_maps",
    "iscsi_target_node_add_pg_ig_maps",
    "iscsi_create_target_node",
    "iscsi_get_target_nodes",
    "iscsi_delete_initiator_group",
    "iscsi_initiator_group_remove_initiators",
    "iscsi_initiator_group_add_initiators",
    "iscsi_create_initiator_group",
    "iscsi_get_initiator_groups",
    "iscsi_set_options",
    "bdev_set_options",
    "bdev_set_qos_limit",
    "bdev_get_bdevs",
    "bdev_get_iostat",
    "framework_get_config",
    "framework_get_subsystems",
    "framework_monitor_context_switch",
    "spdk_kill_instance",
    "ioat_scan_accel_engine",
    "idxd_scan_accel_engine",
    "bdev_virtio_attach_controller",
    "bdev_virtio_scsi_get_devices",
    "bdev_virtio_detach_controller",
    "bdev_virtio_blk_set_hotplug",
    "bdev_aio_delete",
    "bdev_aio_create",
    "bdev_split_delete",
    "bdev_split_create",
    "bdev_error_inject_error",
    "bdev_error_delete",
    "bdev_error_create",
    "bdev_passthru_create",
    "bdev_passthru_delete"
    "bdev_nvme_apply_firmware",
    "bdev_nvme_get_transport_statistics",
    "bdev_nvme_get_controller_health_info",
    "bdev_nvme_detach_controller",
    "bdev_nvme_attach_controller",
    "bdev_null_create",
    "bdev_malloc_delete",
    "bdev_malloc_create",
    "bdev_ftl_delete",
    "bdev_ftl_create",
    "bdev_lvol_get_lvstores",
    "bdev_lvol_delete",
    "bdev_lvol_resize",
    "bdev_lvol_set_read_only",
    "bdev_lvol_decouple_parent",
    "bdev_lvol_inflate",
    "bdev_lvol_rename",
    "bdev_lvol_clone",
    "bdev_lvol_snapshot",
    "bdev_lvol_create",
    "bdev_lvol_delete_lvstore",
    "bdev_lvol_rename_lvstore",
    "bdev_lvol_create_lvstore"
  ]
}
~~~

### framework_get_subsystems {#rpc_framework_get_subsystems}

Get an array of name and dependency relationship of SPDK subsystems in initialization order.

#### Parameters

None

#### Response

The response is an array of name and dependency relationship of SPDK subsystems in initialization order.

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "framework_get_subsystems"
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": [
    {
      "subsystem": "accel",
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
        "accel"
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

### framework_get_config {#rpc_framework_get_config}

Get current configuration of the specified SPDK framework

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | SPDK subsystem name

#### Response

The response is current configuration of the specified SPDK subsystem.
Null is returned if it is not retrievable by the framework_get_config method and empty array is returned if it is empty.

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "framework_get_config",
  "params": {
    "name": "bdev"
  }
}
~~~

Example response:

~~~json
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
      "method": "bdev_split_create"
    },
    {
      "params": {
        "trtype": "PCIe",
        "name": "Nvme1",
        "traddr": "0000:01:00.0"
      },
      "method": "bdev_nvme_attach_controller"
    },
    {
      "params": {
        "trtype": "PCIe",
        "name": "Nvme2",
        "traddr": "0000:03:00.0"
      },
      "method": "bdev_nvme_attach_controller"
    },
    {
      "params": {
        "block_size": 512,
        "num_blocks": 131072,
        "name": "Malloc0",
        "uuid": "913fc008-79a7-447f-b2c4-c73543638c31"
      },
      "method": "bdev_malloc_create"
    },
    {
      "params": {
        "block_size": 512,
        "num_blocks": 131072,
        "name": "Malloc1",
        "uuid": "dd5b8f6e-b67a-4506-b606-7fff5a859920"
      },
      "method": "bdev_malloc_create"
    }
  ]
}
~~~

### framework_get_reactors {#rpc_framework_get_reactors}

Retrieve an array of all reactors.

#### Parameters

This method has no parameters.

#### Response

The response is an array of all reactors.

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "framework_get_reactors",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "tick_rate": 2400000000,
    "reactors": [
      {
        "lcore": 0,
        "busy": 41289723495,
        "idle": 3624832946,
        "lw_threads": [
          {
            "name": "app_thread",
            "id", 1,
            "cpumask": "1",
            "elapsed": 44910853363
          }
        ]
      }
    ]
  }
}
~~~

### framework_set_scheduler {#rpc_framework_set_scheduler}

Select thread scheduler that will be activated.
This feature is considered as experimental.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | Name of a scheduler
period                  | Optional | number      | Scheduler period

#### Response

Completion status of the operation is returned as a boolean.

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "framework_set_scheduler",
  "id": 1,
  "params": {
    "name": "static",
    "period": "1000000"
  }
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### framework_get_scheduler {#rpc_framework_get_scheduler}

Retrieve currently set scheduler name and period, along with current governor name.

#### Parameters

This method has no parameters.

#### Response

Name                    | Description
------------------------| -----------
scheduler_name          | Current scheduler name
scheduler_period        | Currently set scheduler period in microseconds
governor_name           | Governor name

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "framework_set_scheduler",
  "id": 1,
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "scheduler_name": "static",
    "scheduler_period": 2800000000,
    "governor_name": "default"
  }
}
~~~

### thread_get_stats {#rpc_thread_get_stats}

Retrieve current statistics of all the threads.

#### Parameters

This method has no parameters.

#### Response

The response is an array of objects containing threads statistics.

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "thread_get_stats",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "tick_rate": 2400000000,
    "threads": [
      {
        "name": "app_thread",
        "id": 1,
	"cpumask": "1",
        "busy": 139223208,
        "idle": 8641080608,
        "in_interrupt": false,
        "active_pollers_count": 1,
        "timed_pollers_count": 2,
        "paused_pollers_count": 0
      }
    ]
  }
}
~~~

### thread_set_cpumask {#rpc_thread_set_cpumask}

Set the cpumask of the thread to the specified value. The thread may be migrated
to one of the specified CPUs.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
id                      | Required | string      | Thread ID
cpumask                 | Required | string      | Cpumask for this thread

#### Response

Completion status of the operation is returned as a boolean.

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "thread_set_cpumask",
  "id": 1,
  "params": {
    "id": "1",
    "cpumask": "1"
  }
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### trace_enable_tpoint_group {#rpc_trace_enable_tpoint_group}

Enable trace on a specific tpoint group. For example "bdev" for bdev trace group,
"all" for all trace groups.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | bdev, nvmf_rdma, nvmf_tcp, blobfs, scsi, iscsi_conn, ftl, all

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "trace_enable_tpoint_group",
  "id": 1,
  "params": {
    "name": "bdev"
  }
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### trace_disable_tpoint_group {#rpc_trace_disable_tpoint_group}

Disable trace on a specific tpoint group. For example "bdev" for bdev trace group,
"all" for all trace groups.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | bdev, nvmf_rdma, nvmf_tcp, blobfs, all

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "trace_disable_tpoint_group",
  "id": 1,
  "params": {
    "name": "bdev"
  }
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### trace_set_tpoint_mask {#rpc_trace_set_tpoint_mask}

Enable tracepoint mask on a specific tpoint group. For example "bdev" for bdev trace group,
and 0x1 to enable the first tracepoint inside the group (BDEV_IO_START). This command will not
disable already active tracepoints or those not specified in the mask. For a full description
of all available trace groups, see
[tracepoint documentation](https://spdk.io/doc/nvmf_tgt_tracepoints.html).

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | bdev, nvmf_rdma, nvmf_tcp, blobfs, scsi, iscsi_conn, ftl
tpoint_mask             | Required | number      | mask to enable tracepoints inside a group

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "trace_set_tpoint_mask",
  "id": 1,
  "params": {
    "name": "bdev",
    "tpoint_mask": 0x1
  }
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### trace_clear_tpoint_mask {#rpc_trace_clear_tpoint_mask}

Disable tracepoint mask on a specific tpoint group. For example "bdev" for bdev trace group,
and 0x1 to disable the first tracepoint inside the group (BDEV_IO_START). For a full description
of all available trace groups, see
[tracepoint documentation](https://spdk.io/doc/nvmf_tgt_tracepoints.html).

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | bdev, nvmf_rdma, nvmf_tcp, blobfs, scsi, iscsi_conn, ftl
tpoint_mask             | Required | number      | mask to diesable tracepoints inside a group

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "trace_clear_tpoint_mask",
  "id": 1,
  "params": {
    "name": "bdev",
    "tpoint_mask": 0x1
  }
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### trace_get_tpoint_group_mask {#rpc_trace_get_tpoint_group_mask}

Display mask info for every group.

#### Parameters

No parameters required

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "trace_get_tpoint_group_mask",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "tpoint_group_mask": "0x0",
  "iscsi_conn": {
    "enabled": false,
    "mask": "0x2"
  },
  "scsi": {
    "enabled": false,
    "mask": "0x4"
  },
  "bdev": {
    "enabled": false,
    "mask": "0x8"
  },
  "nvmf_tcp": {
    "enabled": false,
    "mask": "0x20"
  },
  "ftl": {
    "enabled": false,
    "mask": "0x40"
  },
  "blobfs": {
    "enabled": false,
    "mask": "0x80"
    }
  }
}
~~~

### log_set_print_level {#rpc_log_set_print_level}

Set the current level at which output will additionally be
sent to the current console.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
level                   | Required | string      | ERROR, WARNING, NOTICE, INFO, DEBUG

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "log_set_print_level",
  "id": 1,
  "params": {
    "level": "ERROR"
  }
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### log_get_print_level {#rpc_log_get_print_level}

Get the current level at which output will additionally be
sent to the current console.

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "log_get_print_level",
  "id": 1,
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": "NOTICE"
}
~~~

### log_set_level {#rpc_log_set_level}

Set the current logging level output by the `log` module.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
level                   | Required | string      | ERROR, WARNING, NOTICE, INFO, DEBUG

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "log_set_level",
  "id": 1,
  "params": {
    "level": "ERROR"
  }
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### log_get_level {#rpc_log_get_level}

Get the current logging level output by the `log` module.

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "log_get_level",
  "id": 1,
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": "NOTICE"
}
~~~

### log_set_flag {#rpc_log_set_flag}

Enable logging for specific portions of the application. The list of possible
log flags can be obtained using the `log_get_flags` RPC and may be different
for each application.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
flag                    | Required | string      | A log flag, or 'all'

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "log_set_flag",
  "id": 1,
  "params": {
    "flag": "all"
  }
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### log_clear_flag {#rpc_log_clear_flag}

Disable logging for specific portions of the application. The list of possible
log flags can be obtained using the `log_get_flags` RPC and may be different
for each application.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
flag                    | Required | string      | A log flag, or 'all'

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "log_clear_flag",
  "id": 1,
  "params": {
    "flag": "all"
  }
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### log_get_flags {#rpc_log_get_flags}

Get the list of valid flags for this application and whether
they are currently enabled.

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "log_get_flags",
  "id": 1,
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "nvmf": true,
    "nvme": true,
    "aio": false,
    "bdev" false
  }
}
~~~

### log_enable_timestamps {#rpc_log_enable_timestamps}

Enable or disable timestamps.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
enabled                 | Required | boolean     | on or off

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "log_enable_timestamps",
  "id": 1,
  "params": {
    "enabled": true
  }
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### thread_get_pollers {#rpc_thread_get_pollers}

Retrieve current pollers of all the threads.

#### Parameters

This method has no parameters.

### Response

The response is an array of objects containing pollers of all the threads.

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "thread_get_pollers",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "tick_rate": 2500000000,
    "threads": [
      {
        "name": "app_thread",
        "id": 1,
        "active_pollers": [],
        "timed_pollers": [
          {
            "name": "spdk_rpc_subsystem_poll",
            "id": 1,
            "state": "waiting",
            "run_count": 12345,
            "busy_count": 10000,
            "period_ticks": 10000000
          }
        ],
        "paused_pollers": []
      }
    ]
  }
}
~~~

### thread_get_io_channels {#rpc_thread_get_io_channels}

Retrieve current IO channels of all the threads.

#### Parameters

This method has no parameters.

#### Response

The response is an array of objects containing IO channels of all the threads.

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "thread_get_io_channels",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "tick_rate": 2500000000,
    "threads": [
      {
        "name": "app_thread",
        "io_channels": [
          {
            "name": "nvmf_tgt",
            "ref": 1
          }
        ]
      }
    ]
  }
}
~~~

### env_dpdk_get_mem_stats {#rpc_env_dpdk_get_mem_stats}

Write the dpdk memory stats to a file.

#### Parameters

This method has no parameters.

#### Response

The response is a pathname to a text file containing the memory stats.

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "env_dpdk_get_mem_stats",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "filename": "/tmp/spdk_mem_dump.txt"
  }
}
~~~

## Acceleration Framework Layer {#jsonrpc_components_accel_fw}

### idxd_scan_accel_engine {#rpc_idxd_scan_accel_engine}

Set config and enable idxd accel engine offload.
This feature is considered as experimental.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
config_number           | Required | number      | Pre-defined config # to use (ie 0, 1). See [docs.](https://spdk.io/doc/idxd.html)
config_kernel_mode      | Optional | Boolean     | If set, will use kernel idxd driver.

#### Example

Example request:

~~~json
{
  "params": {
    "config_number": 0,
    "config_kernel_mode": false
  },
  "jsonrpc": "2.0",
  "method": "idxd_scan_accel_engine",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### ioat_scan_accel_engine {#rpc_ioat_scan_accel_engine}

Enable ioat accel engine offload.

#### Parameters

None

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "ioat_scan_accel_engine",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

## Block Device Abstraction Layer {#jsonrpc_components_bdev}

### bdev_set_options {#rpc_bdev_set_options}

Set global parameters for the block device (bdev) subsystem.  This RPC may only be called
before SPDK subsystems have been initialized.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
bdev_io_pool_size       | Optional | number      | Number of spdk_bdev_io structures in shared buffer pool
bdev_io_cache_size      | Optional | number      | Maximum number of spdk_bdev_io structures cached per thread
bdev_auto_examine       | Optional | boolean     | If set to false, the bdev layer will not examine every disks automatically

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "bdev_set_options",
  "params": {
    "bdev_io_pool_size": 65536,
    "bdev_io_cache_size": 256
  }
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### bdev_get_bdevs {#rpc_bdev_get_bdevs}

Get information about block devices (bdevs).

#### Parameters

The user may specify no parameters in order to list all block devices, or a block device may be
specified by name.

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Optional | string      | Block device name

#### Response

The response is an array of objects containing information about the requested block devices.

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "bdev_get_bdevs",
  "params": {
    "name": "Malloc0"
  }
}
~~~

Example response:

~~~json
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
      "zoned": false,
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

### bdev_examine {#rpc_bdev_examine}

Request that the bdev layer examines the given bdev for metadata and creates
new bdevs if metadata is found. This is only necessary if `auto_examine` has
been set to false using `bdev_set_options`. By default, `auto_examine` is true
and bdev examination is automatic.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | Block device name

#### Response

The response is an array of objects containing I/O statistics of the requested block devices.

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "bdev_examine",
  "params": {
    "name": "Nvme0n1"
  }
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### bdev_wait_for_examine {#rpc_bdev_wait_for_examine}

Report when all bdevs have been examined by every bdev module.

#### Parameters

None

#### Response

The response is sent when all bdev modules had a chance to examine every bdev.

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "bdev_wait_for_examine",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### bdev_get_iostat {#rpc_bdev_get_iostat}

Get I/O statistics of block devices (bdevs).

#### Parameters

The user may specify no parameters in order to list all block devices, or a block device may be
specified by name.

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Optional | string      | Block device name

#### Response

The response is an array of objects containing I/O statistics of the requested block devices.

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "bdev_get_iostat",
  "params": {
    "name": "Nvme0n1"
  }
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "tick_rate": 2200000000,
    "bdevs" : [
      {
        "name": "Nvme0n1",
        "bytes_read": 36864,
        "num_read_ops": 2,
        "bytes_written": 0,
        "num_write_ops": 0,
        "bytes_unmapped": 0,
        "num_unmap_ops": 0,
        "read_latency_ticks": 178904,
        "write_latency_ticks": 0,
        "unmap_latency_ticks": 0,
        "queue_depth_polling_period": 2,
        "queue_depth": 0,
        "io_time": 0,
        "weighted_io_time": 0
      }
    ]
  }
}
~~~

### bdev_enable_histogram {#rpc_bdev_enable_histogram}

Control whether collecting data for histogram is enabled for specified bdev.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | Block device name
enable                  | Required | boolean     | Enable or disable histogram on specified device

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "bdev_enable_histogram",
  "params": {
    "name": "Nvme0n1"
    "enable": true
  }
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### bdev_get_histogram {#rpc_bdev_get_histogram}

Get latency histogram for specified bdev.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | Block device name

#### Result

Name                    | Description
------------------------| -----------
histogram               | Base64 encoded histogram
bucket_shift            | Granularity of the histogram buckets
tsc_rate                | Ticks per second

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "bdev_get_histogram",
  "params": {
    "name": "Nvme0n1"
  }
}
~~~

Example response:
Note that histogram field is trimmed, actual encoded histogram length is ~80kb.

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "histogram": "AAAAAAAAAAAAAA...AAAAAAAAA==",
    "tsc_rate": 2300000000,
    "bucket_shift": 7
  }
}
~~~

### bdev_set_qos_limit {#rpc_bdev_set_qos_limit}

Set the quality of service rate limit on a bdev.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | Block device name
rw_ios_per_sec          | Optional | number      | Number of R/W I/Os per second to allow. 0 means unlimited.
rw_mbytes_per_sec       | Optional | number      | Number of R/W megabytes per second to allow. 0 means unlimited.
r_mbytes_per_sec        | Optional | number      | Number of Read megabytes per second to allow. 0 means unlimited.
w_mbytes_per_sec        | Optional | number      | Number of Write megabytes per second to allow. 0 means unlimited.

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "bdev_set_qos_limit",
  "params": {
    "name": "Malloc0"
    "rw_ios_per_sec": 20000
    "rw_mbytes_per_sec": 100
    "r_mbytes_per_sec": 50
    "w_mbytes_per_sec": 50
  }
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### bdev_set_qd_sampling_period {#rpc_bdev_set_qd_sampling_period}

Enable queue depth tracking on a specified bdev.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | Block device name
period                  | Required | int         | period (in microseconds).If set to 0, polling will be disabled.

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "bdev_set_qd_sampling_period",
  "id": 1,
  "params": {
    "name": "Malloc0",
    "period": 20
  }
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### bdev_compress_create {#rpc_bdev_compress_create}

Create a new compress bdev on a given base bdev.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
base_bdev_name          | Required | string      | Name of the base bdev
pm_path                 | Required | string      | Path to persistent memory
lb_size                 | Optional | int         | Compressed vol logical block size (512 or 4096)

#### Result

Name of newly created bdev.

#### Example

Example request:

~~~json
{
  "params": {
    "base_bdev_name": "Nvme0n1",
    "pm_path": "/pm_files",
    "lb_size": 4096
  },
  "jsonrpc": "2.0",
  "method": "bdev_compress_create",
  "id": 1
}
~~~

### bdev_compress_delete {#rpc_bdev_compress_delete}

Delete a compressed bdev.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | Name of the compress bdev

#### Example

Example request:

~~~json
{
  "params": {
    "name": "COMP_Nvme0n1"
  },
  "jsonrpc": "2.0",
  "method": "bdev_compress_delete",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### bdev_compress_get_orphans {#rpc_bdev_compress_get_orphans}

Get a list of compressed volumes that are missing their pmem metadata.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | Name of the compress bdev

#### Example

Example request:

~~~json
{
  "params": {
    "name": "COMP_Nvme0n1"
  },
  "jsonrpc": "2.0",
  "method": "bdev_compress_get_orphans",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "name": "COMP_Nvme0n1"
}
~~~

### bdev_compress_set_pmd {#rpc_bdev_compress_set_pmd}

Select the DPDK polled mode driver (pmd) for a compressed bdev,
0 = auto-select, 1= QAT only, 2 = ISAL only, 3 = mlx5_pci only.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
pmd                     | Required | int         | pmd selection

#### Example

Example request:

~~~json
{
  "params": {
    "pmd": 1
  },
  "jsonrpc": "2.0",
  "method": "bdev_compress_set_pmd",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### bdev_crypto_create {#rpc_bdev_crypto_create}

Create a new crypto bdev on a given base bdev.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
base_bdev_name          | Required | string      | Name of the base bdev
name                    | Required | string      | Name of the crypto vbdev to create
crypto_pmd              | Required | string      | Name of the crypto device driver
key                     | Required | string      | Key
cipher                  | Required | string      | Cipher to use, AES_CBC or AES_XTS (QAT only)
key2                    | Required | string      | 2nd key only required for cipher AET_XTS

#### Result

Name of newly created bdev.

#### Example

Example request:

~~~json
{
  "params": {
    "base_bdev_name": "Nvme0n1",
    "name": "my_crypto_bdev",
    "crypto_pmd": "crypto_aesni_mb",
    "key": "1234567890123456",
    "cipher": "AES_CBC"
  },
  "jsonrpc": "2.0",
  "method": "bdev_crypto_create",
  "id": 1
}
~~~

Example response:

~~~json
{

  "jsonrpc": "2.0",
  "id": 1,
  "result": "my_crypto_bdev"
}
~~~

### bdev_crypto_delete {#rpc_bdev_crypto_delete}

Delete a crypto bdev.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | Name of the crypto bdev

#### Example

Example request:

~~~json
{
  "params": {
    "name": "my_crypto_bdev"
  },
  "jsonrpc": "2.0",
  "method": "bdev_crypto_delete",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### bdev_ocf_create {#rpc_bdev_ocf_create}

Construct new OCF bdev.
Command accepts cache mode that is going to be used.
You can find more details about supported cache modes in the [OCF documentation](https://open-cas.github.io/cache_configuration.html#cache-mode)

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | Bdev name to use
mode                    | Required | string      | OCF cache mode: wb, wt, pt, wa, wi, wo
cache_line_size         | Optional | int         | OCF cache line size in KiB: 4, 8, 16, 32, 64
cache_bdev_name         | Required | string      | Name of underlying cache bdev
core_bdev_name          | Required | string      | Name of underlying core bdev

#### Result

Name of newly created bdev.

#### Example

Example request:

~~~json
{
  "params": {
    "name": "ocf0",
    "mode": "wt",
    "cache_line_size": 64,
    "cache_bdev_name": "Nvme0n1",
    "core_bdev_name": "aio0"
  },
  "jsonrpc": "2.0",
  "method": "bdev_ocf_create",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": "ocf0"
}
~~~

### bdev_ocf_delete {#rpc_bdev_ocf_delete}

Delete the OCF bdev

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | Bdev name

#### Example

Example request:

~~~json
{
  "params": {
    "name": "ocf0"
  },
  "jsonrpc": "2.0",
  "method": "bdev_ocf_delete",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### bdev_ocf_get_stats {#rpc_bdev_ocf_get_stats}

Get statistics of chosen OCF block device.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | Block device name

#### Response

Statistics as json object.

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "bdev_ocf_get_stats",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": [
  "usage": {
    "clean": {
      "count": 76033,
      "units": "4KiB blocks",
      "percentage": "100.0"
    },
    "free": {
      "count": 767,
      "units": "4KiB blocks",
      "percentage": "0.9"
    },
    "occupancy": {
      "count": 76033,
      "units": "4KiB blocks",
      "percentage": "99.0"
    },
    "dirty": {
      "count": 0,
      "units": "4KiB blocks",
      "percentage": "0.0"
    }
  },
  "requests": {
    "rd_total": {
      "count": 2,
      "units": "Requests",
      "percentage": "0.0"
    },
    "wr_full_misses": {
      "count": 76280,
      "units": "Requests",
      "percentage": "35.6"
    },
    "rd_full_misses": {
      "count": 1,
      "units": "Requests",
      "percentage": "0.0"
    },
    "rd_partial_misses": {
      "count": 0,
      "units": "Requests",
      "percentage": "0.0"
    },
    "wr_total": {
      "count": 212416,
      "units": "Requests",
      "percentage": "99.2"
    },
    "wr_pt": {
      "count": 1535,
      "units": "Requests",
      "percentage": "0.7"
    },
    "wr_partial_misses": {
      "count": 0,
      "units": "Requests",
      "percentage": "0.0"
    },
    "serviced": {
      "count": 212418,
      "units": "Requests",
      "percentage": "99.2"
    },
    "rd_pt": {
      "count": 0,
      "units": "Requests",
      "percentage": "0.0"
    },
    "total": {
      "count": 213953,
      "units": "Requests",
      "percentage": "100.0"
    },
    "rd_hits": {
      "count": 1,
      "units": "Requests",
      "percentage": "0.0"
    },
    "wr_hits": {
      "count": 136136,
      "units": "Requests",
      "percentage": "63.6"
    }
  },
  "errors": {
    "total": {
      "count": 0,
      "units": "Requests",
      "percentage": "0.0"
    },
    "cache_obj_total": {
      "count": 0,
      "units": "Requests",
      "percentage": "0.0"
    },
    "core_obj_total": {
      "count": 0,
      "units": "Requests",
      "percentage": "0.0"
    },
    "cache_obj_rd": {
      "count": 0,
      "units": "Requests",
      "percentage": "0.0"
    },
    "core_obj_wr": {
      "count": 0,
      "units": "Requests",
      "percentage": "0.0"
    },
    "core_obj_rd": {
      "count": 0,
      "units": "Requests",
      "percentage": "0.0"
    },
    "cache_obj_wr": {
      "count": 0,
      "units": "Requests",
      "percentage": "0.0"
    }
  },
  "blocks": {
    "volume_rd": {
      "count": 9,
      "units": "4KiB blocks",
      "percentage": "0.0"
    },
    "volume_wr": {
      "count": 213951,
      "units": "4KiB blocks",
      "percentage": "99.9"
    },
    "cache_obj_total": {
      "count": 212425,
      "units": "4KiB blocks",
      "percentage": "100.0"
    },
    "core_obj_total": {
      "count": 213959,
      "units": "4KiB blocks",
      "percentage": "100.0"
    },
    "cache_obj_rd": {
      "count": 1,
      "units": "4KiB blocks",
      "percentage": "0.0"
    },
    "core_obj_wr": {
      "count": 213951,
      "units": "4KiB blocks",
      "percentage": "99.9"
    },
    "volume_total": {
      "count": 213960,
      "units": "4KiB blocks",
      "percentage": "100.0"
    },
    "core_obj_rd": {
      "count": 8,
      "units": "4KiB blocks",
      "percentage": "0.0"
    },
    "cache_obj_wr": {
      "count": 212424,
      "units": "4KiB blocks",
      "percentage": "99.9"
    }
  ]
}
~~~

### bdev_ocf_get_bdevs {#rpc_bdev_ocf_get_bdevs}

Get list of OCF devices including unregistered ones.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Optional | string      | Name of OCF vbdev or name of cache device or name of core device

#### Response

Array of OCF devices with their current status, along with core and cache bdevs.

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "bdev_ocf_get_bdevs",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": [
    {
      "name": "PartCache",
      "started": false,
      "cache": {
        "name": "Malloc0",
        "attached": true
      },
      "core": {
        "name": "Malloc1",
        "attached": false
      }
    }
  ]
}
~~~

### bdev_ocf_set_cache_mode {#rpc_bdev_ocf_set_cache_mode}

Set new cache mode on OCF bdev.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | Bdev name
mode                    | Required | string      | OCF cache mode: wb, wt, pt, wa, wi, wo

#### Response

New cache mode name.

#### Example

Example request:

~~~json
{
  "params": {
    "name": "ocf0",
    "mode": "pt",
  },
  "jsonrpc": "2.0",
  "method": "bdev_ocf_set_cache_mode",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": "pt"
}
~~~

### bdev_malloc_create {#rpc_bdev_malloc_create}

Construct @ref bdev_config_malloc

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Optional | string      | Bdev name to use
block_size              | Required | number      | Block size in bytes -must be multiple of 512
num_blocks              | Required | number      | Number of blocks
uuid                    | Optional | string      | UUID of new bdev
optimal_io_boundary     | Optional | number      | Split on optimal IO boundary, in number of blocks, default 0

#### Result

Name of newly created bdev.

#### Example

Example request:

~~~json
{
  "params": {
    "block_size": 4096,
    "num_blocks": 16384,
    "name": "Malloc0",
    "uuid": "2b6601ba-eada-44fb-9a83-a20eb9eb9e90",
    "optimal_io_boundary": 16
  },
  "jsonrpc": "2.0",
  "method": "bdev_malloc_create",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": "Malloc0"
}
~~~

### bdev_malloc_delete {#rpc_bdev_malloc_delete}

Delete @ref bdev_config_malloc

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | Bdev name

#### Example

Example request:

~~~json
{
  "params": {
    "name": "Malloc0"
  },
  "jsonrpc": "2.0",
  "method": "bdev_malloc_delete",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### bdev_null_create {#rpc_bdev_null_create}

Construct @ref bdev_config_null

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Optional | string      | Bdev name to use
block_size              | Required | number      | Block size in bytes
num_blocks              | Required | number      | Number of blocks
uuid                    | Optional | string      | UUID of new bdev
md_size                 | Optional | number      | Metadata size for this bdev. Default=0.
dif_type                | Optional | number      | Protection information type. Parameter --md-size needs to be set along --dif-type. Default=0 - no protection.
dif_is_head_of_md       | Optional | boolean     | Protection information is in the first 8 bytes of metadata. Default=false.

#### Result

Name of newly created bdev.

#### Example

Example request:

~~~json
{
  "params": {
    "block_size": 4104,
    "num_blocks": 16384,
    "name": "Null0",
    "uuid": "2b6601ba-eada-44fb-9a83-a20eb9eb9e90",
    "md_size": 8,
    "dif_type": 1,
    "dif_is_head_of_md": true
  },
  "jsonrpc": "2.0",
  "method": "bdev_null_create",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": "Null0"
}
~~~

### bdev_null_delete {#rpc_bdev_null_delete}

Delete @ref bdev_config_null.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | Bdev name

#### Example

Example request:

~~~json
{
  "params": {
    "name": "Null0"
  },
  "jsonrpc": "2.0",
  "method": "bdev_null_delete",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### bdev_null_resize {#rpc_bdev_null_resize}

Resize @ref bdev_config_null.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | Bdev name
new_size                | Required | number      | Bdev new capacity in MB

#### Example

Example request:

~~~json
{
  "params": {
    "name": "Null0",
    "new_size": 4096
  },
  "jsonrpc": "2.0",
  "method": "bdev_null_resize",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### bdev_aio_create {#rpc_bdev_aio_create}

Construct @ref bdev_config_aio.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | Bdev name to use
filename                | Required | number      | Path to device or file
block_size              | Optional | number      | Block size in bytes

#### Result

Name of newly created bdev.

#### Example

Example request:

~~~json
{
  "params": {
    "block_size": 4096,
    "name": "Aio0",
    "filename": "/tmp/aio_bdev_file"
  },
  "jsonrpc": "2.0",
  "method": "bdev_aio_create",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": "Aio0"
}
~~~

### bdev_aio_rescan {#rpc_bdev_aio_rescan}

Rescan the size of @ref bdev_config_aio.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | Bdev name

#### Example

Example request:

~~json
{
  "params": {
    "name": "Aio0"
  },
  "jsonrpc": "2.0",
  "method": "bdev_aio_rescan",
  "id": 1
}
~~

Example response:

~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~

### bdev_aio_delete {#rpc_bdev_aio_delete}

Delete @ref bdev_config_aio.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | Bdev name

#### Example

Example request:

~~~json
{
  "params": {
    "name": "Aio0"
  },
  "jsonrpc": "2.0",
  "method": "bdev_aio_delete",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### bdev_nvme_set_options {#rpc_bdev_nvme_set_options}

Set global parameters for all bdev NVMe. This RPC may only be called before SPDK subsystems have been initialized
or any bdev NVMe has been created.

#### Parameters

Name                       | Optional | Type        | Description
-------------------------- | -------- | ----------- | -----------
action_on_timeout          | Optional | string      | Action to take on command time out: none, reset or abort
timeout_us                 | Optional | number      | Timeout for each command, in microseconds. If 0, don't track timeouts
timeout_admin_us           | Optional | number      | Timeout for each admin command, in microseconds. If 0, treat same as io timeouts ('timeout_us')
keep_alive_timeout_ms      | Optional | number      | Keep alive timeout period in milliseconds, default is 10s
retry_count                | Optional | number      | The number of attempts per I/O before an I/O fails. (Deprecated. Please use transport_retry_count instead.)
arbitration_burst          | Optional | number      | The value is expressed as a power of two, a value of 111b indicates no limit
low_priority_weight        | Optional | number      | The maximum number of commands that the controller may launch at one time from a low priority queue
medium_priority_weight     | Optional | number      | The maximum number of commands that the controller may launch at one time from a medium priority queue
high_priority_weight       | Optional | number      | The maximum number of commands that the controller may launch at one time from a high priority queue
nvme_adminq_poll_period_us | Optional | number      | How often the admin queue is polled for asynchronous events in microseconds
nvme_ioq_poll_period_us    | Optional | number      | How often I/O queues are polled for completions, in microseconds. Default: 0 (as fast as possible).
io_queue_requests          | Optional | number      | The number of requests allocated for each NVMe I/O queue. Default: 512.
delay_cmd_submit           | Optional | boolean     | Enable delaying NVMe command submission to allow batching of multiple commands. Default: `true`.
transport_retry_count      | Optional | number      | The number of attempts per I/O in the transport layer before an I/O fails.
bdev_retry_count           | Optional | number      | The number of attempts per I/O in the bdev layer before an I/O fails. -1 means infinite retries.

#### Example

Example request:

~~~json
request:
{
  "params": {
    "transport_retry_count": 5,
    "arbitration_burst": 3,
    "low_priority_weight": 8,
    "medium_priority_weight":8,
    "high_priority_weight": 8,
    "nvme_adminq_poll_period_us": 2000,
    "timeout_us": 10000000,
    "timeout_admin_us": 20000000,
    "keep_alive_timeout_ms": 600000,
    "action_on_timeout": "reset",
    "io_queue_requests" : 2048,
    "delay_cmd_submit": true
  },
  "jsonrpc": "2.0",
  "method": "bdev_nvme_set_options",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### bdev_nvme_set_hotplug {#rpc_bdev_nvme_set_hotplug}

Change settings of the NVMe hotplug feature. If enabled, PCIe NVMe bdevs will be automatically discovered on insertion
and deleted on removal.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
enabled                 | Required | string      | True to enable, false to disable
period_us               | Optional | number      | How often to poll for hot-insert and hot-remove events. Values: 0 - reset/use default or 1 to 10000000.

#### Example

Example request:

~~~json
request:
{
  "params": {
    "enabled": true,
    "period_us": 2000
  },
  "jsonrpc": "2.0",
  "method": "bdev_nvme_set_hotplug",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### bdev_nvme_attach_controller {#rpc_bdev_nvme_attach_controller}

Construct @ref bdev_config_nvme. This RPC can also be used to add additional paths to an existing controller to enable
multipathing. This is done by specifying the `name` parameter as an existing controller. When adding an additional
path, the hostnqn, hostsvcid, hostaddr, prchk_reftag, and prchk_guard_arguments must not be specified and are assumed
to have the same value as the existing path.

The parameters, `ctrlr_loss_timeout_sec`, `reconnect_delay_sec`, and `fast_io_fail_timeout_sec`, are mutually dependent.
If `reconnect_delay_sec` is non-zero, `ctrlr_loss_timeout_sec` has to be -1 or not less than `reconnect_delay_sec`.
If `reconnect_delay_sec` is zero, `ctrlr_loss_timeout_sec` has to be zero.
If `fast_io_fail_timeout_sec` is not zero, it has to be not less than `reconnect_delay_sec` and less than `ctrlr_loss_timeout_sec` if `ctrlr_loss_timeout_sec` is not -1.

#### Result

Array of names of newly created bdevs.

#### Parameters

Name                       | Optional | Type        | Description
-------------------------- | -------- | ----------- | -----------
name                       | Required | string      | Name of the NVMe controller, prefix for each bdev name
trtype                     | Required | string      | NVMe-oF target trtype: rdma or pcie
traddr                     | Required | string      | NVMe-oF target address: ip or BDF
adrfam                     | Optional | string      | NVMe-oF target adrfam: ipv4, ipv6, ib, fc, intra_host
trsvcid                    | Optional | string      | NVMe-oF target trsvcid: port number
subnqn                     | Optional | string      | NVMe-oF target subnqn
hostnqn                    | Optional | string      | NVMe-oF target hostnqn
hostaddr                   | Optional | string      | NVMe-oF host address: ip address
hostsvcid                  | Optional | string      | NVMe-oF host trsvcid: port number
prchk_reftag               | Optional | bool        | Enable checking of PI reference tag for I/O processing
prchk_guard                | Optional | bool        | Enable checking of PI guard for I/O processing
hdgst                      | Optional | bool        | Enable TCP header digest
ddgst                      | Optional | bool        | Enable TCP data digest
fabrics_connect_timeout_us | Optional | bool        | Timeout for fabrics connect (in microseconds)
multipath                  | Optional | string      | Multipathing behavior: disable, failover, multipath. Default is failover.
num_io_queues              | Optional | uint32_t    | The number of IO queues to request during initialization. Range: (0, UINT16_MAX + 1], Default is 1024.
ctrlr_loss_timeout_sec     | Optional | number      | Time to wait until ctrlr is reconnected before deleting ctrlr.  -1 means infinite reconnects. 0 means no reconnect.
reconnect_delay_sec        | Optional | number      | Time to delay a reconnect trial. 0 means no reconnect.
fast_io_fail_timeout_sec   | Optional | number      | Time to wait until ctrlr is reconnected before failing I/O to ctrlr. 0 means no such timeout.

#### Example

Example request:

~~~json
{
  "params": {
    "trtype": "pcie",
    "name": "Nvme0",
    "traddr": "0000:0a:00.0"
  },
  "jsonrpc": "2.0",
  "method": "bdev_nvme_attach_controller",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": [
    "Nvme0n1"
  ]
}
~~~

### bdev_nvme_get_controllers {#rpc_bdev_nvme_get_controllers}

Get information about NVMe controllers.

#### Parameters

The user may specify no parameters in order to list all NVMe controllers, or one NVMe controller may be
specified by name.

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Optional | string      | NVMe controller name

#### Response

The response is an array of objects containing information about the requested NVMe controllers.

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "bdev_nvme_get_controllers",
  "params": {
    "name": "Nvme0"
  }
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": [
    {
      "name": "Nvme0",
      "trid": {
        "trtype": "PCIe",
        "traddr": "0000:05:00.0"
      }
    }
  ]
}
~~~

### bdev_nvme_detach_controller {#rpc_bdev_nvme_detach_controller}

Detach NVMe controller and delete any associated bdevs. Optionally,
If all of the transport ID options are specified, only remove that
transport path from the specified controller. If that is the only
available path for the controller, this will also result in the
controller being detached and the associated bdevs being deleted.

returns true if the controller and bdevs were successfully destroyed
or the address was properly removed, false otherwise.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | Controller name
trtype                  | Optional | string      | NVMe-oF target trtype: rdma or tcp
traddr                  | Optional | string      | NVMe-oF target address: ip or BDF
adrfam                  | Optional | string      | NVMe-oF target adrfam: ipv4, ipv6, ib, fc, intra_host
trsvcid                 | Optional | string      | NVMe-oF target trsvcid: port number
subnqn                  | Optional | string      | NVMe-oF target subnqn
hostaddr                | Optional | string      | NVMe-oF host address: ip
hostsvcid               | Optional | string      | NVMe-oF host svcid: port number

#### Example

Example requests:

~~~json
{
  "params": {
    "name": "Nvme0"
  },
  "jsonrpc": "2.0",
  "method": "bdev_nvme_detach_controller",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### bdev_nvme_reset_controller {#rpc_bdev_nvme_reset_controller}

Reset NVMe controller.

Returns true if the controller reset was successful, false otherwise.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | NVMe controller name

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "bdev_nvme_reset_controller",
  "params": {
    "name": "Nvme0"
  }
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### bdev_nvme_start_discovery {#rpc_bdev_nvme_start_discovery}

Start a discovery service for the discovery subsystem of the specified transport ID.

The discovery service will read the discovery log page for the specified
discovery subsystem, and automatically attach to any subsystems found in the
log page. When determining a controller name to use when attaching, it will use
the 'name' parameter as a prefix, followed by a unique integer for that discovery
service. If the discovery service identifies a subsystem that has been previously
attached but is listed with a different path, it will use the same controller name
as the previous entry, and connect as a multipath.

When the discovery service sees that a subsystem entry has been removed
from the log page, it will automatically detach from that controller as well.

The 'name' is also used to later stop the discovery service.

#### Parameters

Name                       | Optional | Type        | Description
-------------------------- | -------- | ----------- | -----------
name                       | Required | string      | Prefix for NVMe controllers
trtype                     | Required | string      | NVMe-oF target trtype: rdma or tcp
traddr                     | Required | string      | NVMe-oF target address: ip
adrfam                     | Optional | string      | NVMe-oF target adrfam: ipv4, ipv6
trsvcid                    | Optional | string      | NVMe-oF target trsvcid: port number
hostnqn                    | Optional | string      | NVMe-oF target hostnqn

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "bdev_nvme_start_discovery",
  "id": 1,
  "params": {
    "name": "nvme_auto",
    "trtype": "tcp",
    "traddr": "127.0.0.1",
    "hostnqn": "nqn.2021-12.io.spdk:host1",
    "adrfam": "ipv4",
    "trsvcid": "4420"
  }
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### bdev_nvme_stop_discovery {#rpc_bdev_nvme_stop_discovery}

Stop a discovery service. This includes detaching any controllers that were
discovered via the service that is being stopped.

#### Parameters

Name                       | Optional | Type        | Description
-------------------------- | -------- | ----------- | -----------
name                       | Required | string      | Name of service to stop

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "bdev_nvme_stop_discovery",
  "id": 1,
  "params": {
    "name": "nvme_auto"
  }
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### bdev_nvme_cuse_register {#rpc_bdev_nvme_cuse_register}

Register CUSE device on NVMe controller.
This feature is considered as experimental.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | Name of the NVMe controller
dev_path                | Required | string      | Path to the CUSE controller device, e.g. spdk/nvme0

#### Example

Example request:

~~~json
{
  "params": {
    "dev_path": "spdk/nvme0",
    "name": "Nvme0"
  },
  "jsonrpc": "2.0",
  "method": "bdev_nvme_cuse_register",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### bdev_nvme_cuse_unregister {#rpc_bdev_nvme_cuse_unregister}

Unregister CUSE device on NVMe controller.
This feature is considered as experimental.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | Name of the NVMe controller

#### Example

Example request:

~~~json
{
  "params": {
    "name": "Nvme0"
  },
  "jsonrpc": "2.0",
  "method": "bdev_nvme_cuse_unregister",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### bdev_zone_block_create {#rpc_bdev_zone_block_create}

Creates a virtual zone device on top of existing non-zoned bdev.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | Name of the Zone device
base_bdev               | Required | string      | Name of the Base bdev
zone_capacity           | Required | number      | Zone capacity in blocks
optimal_open_zones      | Required | number      | Number of zones required to reach optimal write speed

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "bdev_zone_block_create",
  "id": 1,
  "params": {
    "name": "zone1",
    "base_bdev": "NVMe0n1",
    "zone_capacity": 4096,
    "optimal_open_zones": 32
  }
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": "zone1"
}
~~~

### bdev_zone_block_delete {#rpc_bdev_zone_block_delete}

Deletes a virtual zone device.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | Name of the Zone device

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "bdev_zone_block_delete",
  "id": 1,
  "params": {
    "name": "zone1"
  }
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### bdev_nvme_apply_firmware {#rpc_bdev_nvme_apply_firmware}

Download and commit firmware to NVMe device.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
filename                | Required | string      | filename of the firmware to download
bdev_name               | Required | string      | Name of the NVMe block device

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "bdev_nvme_apply_firmware",
  "id": 1,
  "params": {
    "filename": "firmware_file",
    "bdev_name": "NVMe0n1"
  }
}
~~~

### bdev_nvme_get_transport_statistics {#rpc_bdev_nvme_get_transport_statistics}

Get bdev_nvme poll group transport statistics.

#### Parameters

This RPC method accepts no parameters

#### Response

The response is an array of objects containing information about transport statistics per NVME poll group.

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "bdev_nvme_get_transport_statistics",
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
	"result": {
	  "poll_groups": [
		{
		  "thread": "nvmf_tgt_poll_group_0",
		  "transports": [
			{
			  "trname": "RDMA",
			  "devices": [
				{
				  "dev_name": "mlx5_1",
				  "polls": 137492169,
				  "idle_polls": 137492169,
				  "completions": 0,
				  "queued_requests": 0,
				  "total_send_wrs": 0,
				  "send_sq_doorbell_updates": 0,
				  "total_recv_wrs": 0,
				  "recv_sq_doorbell_updates": 0
				},
				{
				  "dev_name": "mlx5_0",
				  "polls": 137985185,
				  "idle_polls": 137492169,
				  "completions": 1474593,
				  "queued_requests": 0,
				  "total_send_wrs": 1474593,
				  "send_sq_doorbell_updates": 426147,
				  "total_recv_wrs": 1474721,
				  "recv_sq_doorbell_updates": 348445
				}
			  ]
			},
			{
			  "trname": "PCIE",
			  "polls": 435419831,
			  "idle_polls": 434901004,
			  "completions": 1485543,
			  "cq_doorbell_updates": 518827,
			  "queued_requests": 0,
			  "submitted_requests": 1485543,
			  "sq_doobell_updates": 516081
			}
		  ]
		},
		{
		  "thread": "nvmf_tgt_poll_group_1",
		  "transports": [
			{
			  "trname": "RDMA",
			  "devices": [
				{
				  "dev_name": "mlx5_1",
				  "polls": 140245630,
				  "idle_polls": 140245630,
				  "completions": 0,
				  "queued_requests": 0,
				  "total_send_wrs": 0,
				  "send_sq_doorbell_updates": 0,
				  "total_recv_wrs": 0,
				  "recv_sq_doorbell_updates": 0
				},
				{
				  "dev_name": "mlx5_0",
				  "polls": 140751844,
				  "idle_polls": 140245630,
				  "completions": 1489298,
				  "queued_requests": 0,
				  "total_send_wrs": 1489298,
				  "send_sq_doorbell_updates": 433510,
				  "total_recv_wrs": 1489426,
				  "recv_sq_doorbell_updates": 357956
				}
			  ]
			},
			{
			  "trname": "PCIE",
			  "polls": 429044294,
			  "idle_polls": 428525658,
			  "completions": 1478730,
			  "cq_doorbell_updates": 518636,
			  "queued_requests": 0,
			  "submitted_requests": 1478730,
			  "sq_doobell_updates": 511658
			}
		  ]
		}
	  ]
	}
}
~~~

### bdev_nvme_get_controller_health_info {#rpc_bdev_nvme_get_controller_health_info}

Display health log of the required NVMe bdev device.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | Name of the NVMe bdev controller

#### Response

The response is the object containing information about health log of the NVMe controller.

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "bdev_nvme_get_controller_health_info",
  "id": 1,
  "params": {
    "name": "Nvme0"
  }
}
~~~

Example response:

~~~json
{
  "model_number": "INTEL SSDPE2KX020T8",
  "serial_number": "BTLJ72430ARH2P0BGN",
  "firmware_revision": "VDV10110",
  "traddr": "0000:08:00.0",
  "temperature_celsius": 32,
  "available_spare_percentage": 99,
  "available_spare_threshold_percentage": 10,
  "percentage_used": 2,
  "data_units_read": 1013408619,
  "data_units_written": 346792685,
  "host_read_commands": 30457773282,
  "host_write_commands": 18949677715,
  "controller_busy_time": 4979,
  "power_cycles": 49,
  "power_on_hours": 31118,
  "unsafe_shutdowns": 18,
  "media_errors": 17,
  "num_err_log_entries": 19,
  "warning_temperature_time_minutes": 0,
  "critical_composite_temperature_time_minutes": 0
}
~~~

### bdev_rbd_register_cluster {#rpc_bdev_rbd_register_cluster}

This method is available only if SPDK was build with Ceph RBD support.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | Registerd Rados cluster object name
user_id                 | Optional | string      | Ceph ID (i.e. admin, not client.admin)
config_param            | Optional | string map  | Explicit librados configuration
config_file             | Optional | string      | File path of libraodos configuration file
key_file                | Optional | string      | File path of libraodos key file

This RPC registers a Rados Cluster object handle which is only known
to rbd module, it uses user_id + config_param or user_id + config_file +
key_file or user_id + config_param + config_file + key_file to identify
a Rados cluster object.

When accessing the Ceph cluster as some user other than "admin" (the
default), the "user_id" has to be set.

The configuration items and secret key can be specified by setting config_param,
config_file and key_file, all of them, or none of them. If only config_param is
passed, all key/value pairs are passed to rados_conf_set to configure cluster access.
In practice, "mon_host" (= list of monitor address+port) and "key" (= the secret key
stored in Ceph keyrings) are enough. If config_file and key_file are specified, they must
exist with all relevant settings for accessing the Ceph cluster. If config_param, config_file
and key_file are specified, get the key/value pairs from config_file first and set to
rados_conf_set function, then set pairs in config_param and keyring in key_file. If nothing
is specified, it will get configuration file and key file from the default location
/etc/ceph/ceph.conf and /etc/ceph/ceph.client.user_id.keyring.

#### Result

Name of newly created Rados cluster object.

#### Example

Example request:

~~
{
  "params": {
    "name": "rbd_cluster",
    "user_id": cinder,
    "config_file": "/root/ceph_conf/ceph.conf",
    "key_file": "/root/ceph_conf/ceph.client.cinder.keyring"
  },
  "jsonrpc": "2.0",
  "method": "bdev_rbd_register_cluster",
  "id": 1
}
~~

Example response:

~~
response:
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": "rbd_cluster"
}
~~

### bdev_rbd_unregister_cluster {#rpc_bdev_rbd_unregister_cluster}

This method is available only if SPDK was build with Ceph RBD support.
If there is still rbd bdev using this cluster, the unregisteration operation
will fail.

#### Result

`true` if Rados cluster object with provided name was deleted or `false` otherwise.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -------------------------
name                    | Required | string      | Rados cluster object name

#### Example

Example request:

~~
{
  "params": {
    "name": "rbd_cluster"
  },
  "jsonrpc": "2.0",
  "method": "bdev_rbd_unregister_cluster",
  "id": 1
}
~~

Example response:

~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~

### bdev_rbd_get_clusters_info {#rpc_bdev_rbd_get_clusters_info}

This method is available only if SPDK was build with Ceph RBD support.

#### Result

Returns the cluster info of the Rados Cluster name if provided. Otherwise, it
returns the cluster info of every registered Raods Cluster name.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -------------------------
name                    | Optional | string      | Rados cluster object name

#### Example

Example request:

~~
{
  "params": {
    "name": "rbd_cluster"
  },
  "jsonrpc": "2.0",
  "method": "bdev_rbd_get_clusters_info",
  "id": 1
}
~~

Example response:

~~
{
  "jsonrpc": "2.0",
  "cluster_name": "rbd_cluster"
}
~~

### bdev_rbd_create {#rpc_bdev_rbd_create}

Create @ref bdev_config_rbd bdev

This method is available only if SPDK was build with Ceph RBD support.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Optional | string      | Bdev name
user_id                 | Optional | string      | Ceph ID (i.e. admin, not client.admin)
pool_name               | Required | string      | Pool name
rbd_name                | Required | string      | Image name
block_size              | Required | number      | Block size
config                  | Optional | string map  | Explicit librados configuration
cluster_name            | Optional | string      | Rados cluster object name created in this module.
uuid                    | Optional | string      | UUID of new bdev

If no config is specified, Ceph configuration files must exist with
all relevant settings for accessing the pool. If a config map is
passed, the configuration files are ignored and instead all key/value
pairs are passed to rados_conf_set to configure cluster access. In
practice, "mon_host" (= list of monitor address+port) and "key" (= the
secret key stored in Ceph keyrings) are enough.

When accessing the image as some user other than "admin" (the
default), the "user_id" has to be set.

If provided with cluster_name option, it will use the Rados cluster object
referenced by the name (created by bdev_rbd_register_cluster RPC) and ignores
"user_id + config" combination to create its own Rados cluster.

#### Result

Name of newly created bdev.

#### Example

Example request with `key` from `/etc/ceph/ceph.client.admin.keyring`:

~~~json
{
  "params": {
    "pool_name": "rbd",
    "rbd_name": "foo",
    "config": {
      "mon_host": "192.168.7.1:6789,192.168.7.2:6789",
      "key": "AQDwf8db7zR1GRAA5k7NKXjS5S5V4mntwUDnGQ==",
    }
    "block_size": 4096,
    "uuid": "76210ea4-7920-40a0-a07b-8992a7443c76"
  },
  "jsonrpc": "2.0",
  "method": "bdev_rbd_create",
  "id": 1
}
~~~

Example response:

~~~json
response:
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": "Ceph0"
}
~~~

Example request with `cluster_name`:

~~
{
  "params": {
    "pool_name": "rbd",
    "rbd_name": "foo",
    "block_size": 4096,
    "cluster_name": "rbd_cluster"
  },
  "jsonrpc": "2.0",
  "method": "bdev_rbd_create",
  "id": 1
}
~~

Example response:

~~
response:
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": "Ceph0"
}
~~

### bdev_rbd_delete {#rpc_bdev_rbd_delete}

Delete @ref bdev_config_rbd bdev

This method is available only if SPDK was build with Ceph RBD support.

#### Result

`true` if bdev with provided name was deleted or `false` otherwise.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | Bdev name

#### Example

Example request:

~~~json
{
  "params": {
    "name": "Rbd0"
  },
  "jsonrpc": "2.0",
  "method": "bdev_rbd_delete",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### bdev_rbd_resize {#rpc_bdev_rbd_resize}

Resize @ref bdev_config_rbd bdev

This method is available only if SPDK was build with Ceph RBD support.

#### Result

`true` if bdev with provided name was resized or `false` otherwise.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | Bdev name
new_size                | Required | int         | New bdev size for resize operation in MiB

#### Example

Example request:

~~~json
{
  "params": {
    "name": "Rbd0"
    "new_size": "4096"
  },
  "jsonrpc": "2.0",
  "method": "bdev_rbd_resize",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### bdev_delay_create {#rpc_bdev_delay_create}

Create delay bdev. This bdev type redirects all IO to it's base bdev and inserts a delay on the completion
path to create an artificial drive latency. All latency values supplied to this bdev should be in microseconds.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | Bdev name
base_bdev_name          | Required | string      | Base bdev name
avg_read_latency        | Required | number      | average read latency (us)
p99_read_latency        | Required | number      | p99 read latency (us)
avg_write_latency       | Required | number      | average write latency (us)
p99_write_latency       | Required | number      | p99 write latency (us)

#### Result

Name of newly created bdev.

#### Example

Example request:

~~~json
{
  "params": {
    "base_bdev_name": "Null0",
    "name": "Delay0",
    "avg_read_latency": "15",
    "p99_read_latency": "50",
    "avg_write_latency": "40",
    "p99_write_latency": "110",
  },
  "jsonrpc": "2.0",
  "method": "bdev_delay_create",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": "Delay0"
}
~~~

### bdev_delay_delete {#rpc_bdev_delay_delete}

Delete delay bdev.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | Bdev name

#### Example

Example request:

~~~json
{
  "params": {
    "name": "Delay0"
  },
  "jsonrpc": "2.0",
  "method": "bdev_delay_delete",
  "id": 1
}

~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### bdev_delay_update_latency {#rpc_bdev_delay_update_latency}

Update a target latency value associated with a given delay bdev. Any currently
outstanding I/O will be completed with the old latency.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
delay_bdev_name         | Required | string      | Name of the delay bdev
latency_type            | Required | string      | One of: avg_read, avg_write, p99_read, p99_write
latency_us              | Required | number      | The new latency value in microseconds

#### Result

Name of newly created bdev.

#### Example

Example request:

~~~json
{
  "params": {
    "delay_bdev_name": "Delay0",
    "latency_type": "avg_read",
    "latency_us": "100",
  },
  "jsonrpc": "2.0",
  "method": "bdev_delay_update_latency",
  "id": 1
}
~~~

Example response:

~~~json
{
  "result": "true"
}
~~~

### bdev_error_create {#rpc_bdev_error_create}

Construct error bdev.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
base_name               | Required | string      | Base bdev name

#### Example

Example request:

~~~json
{
  "params": {
    "base_name": "Malloc0"
  },
  "jsonrpc": "2.0",
  "method": "bdev_error_create",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### bdev_error_delete {#rpc_bdev_error_delete}

Delete error bdev

#### Result

`true` if bdev with provided name was deleted or `false` otherwise.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | Error bdev name

#### Example

Example request:

~~~json
{
  "params": {
    "name": "EE_Malloc0"
  },
  "jsonrpc": "2.0",
  "method": "bdev_error_delete",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### bdev_error_inject_error {#rpc_bdev_error_inject_error}

Inject an error via an error bdev. Create an error bdev on base bdev first. Default 'num'
value is 1 and if 'num' is set to zero, the specified injection is disabled.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | Name of the error injection bdev
io_type                 | Required | string      | io type 'clear' 'read' 'write' 'unmap' 'flush' 'all'
error_type              | Required | string      | error type 'failure' 'pending'
num                     | Optional | int         | the number of commands you want to fail.(default:1)

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "bdev_error_inject_error",
  "id": 1,
  "params": {
    "name": "EE_Malloc0",
    "io_type": "write",
    "error_type": "pending",
    "num": 1
  }
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### bdev_iscsi_create {#rpc_bdev_iscsi_create}

Connect to iSCSI target and create bdev backed by this connection.

This method is available only if SPDK was build with iSCSI initiator support.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | Bdev name
initiator_iqn           | Required | string      | IQN name used during connection
url                     | Required | string      | iSCSI resource URI

#### Result

Name of newly created bdev.

#### Example

Example request:

~~~json
{
  "params": {
    "url": "iscsi://127.0.0.1/iqn.2016-06.io.spdk:disk1/0",
    "initiator_iqn": "iqn.2016-06.io.spdk:init",
    "name": "iSCSI0"
  },
  "jsonrpc": "2.0",
  "method": "bdev_iscsi_create",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": "iSCSI0"
}
~~~

### bdev_iscsi_delete {#rpc_bdev_iscsi_delete}

Delete iSCSI bdev and terminate connection to target.

This method is available only if SPDK was built with iSCSI initiator support.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | Bdev name

#### Example

Example request:

~~~json
{
  "params": {
    "name": "iSCSI0"
  },
  "jsonrpc": "2.0",
  "method": "bdev_iscsi_delete",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### bdev_ftl_create {#rpc_bdev_ftl_create}

Create FTL bdev.

This RPC is subject to change.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | Bdev name
trtype                  | Required | string      | Transport type
traddr                  | Required | string      | NVMe target address
punits                  | Required | string      | Parallel unit range in the form of start-end e.g 4-8
uuid                    | Optional | string      | UUID of restored bdev (not applicable when creating new instance)
cache                   | Optional | string      | Name of the bdev to be used as a write buffer cache

#### Result

Name of newly created bdev.

#### Example

Example request:

~~~json
{
  "params": {
    "name": "nvme0"
    "trtype" "pcie"
    "traddr": "0000:00:04.0"
    "punits": "0-3"
    "uuid": "4a7481ce-786f-41a0-9b86-8f7465c8f4d3"
  },
  "jsonrpc": "2.0",
  "method": "bdev_ftl_create",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
      "name" : "nvme0"
      "uuid" : "4a7481ce-786f-41a0-9b86-8f7465c8f4d3"
  }
}
~~~

### bdev_ftl_delete {#rpc_bdev_ftl_delete}

Delete FTL bdev.

This RPC is subject to change.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | Bdev name

#### Example

Example request:

~~~json
{
  "params": {
    "name": "nvme0"
  },
  "jsonrpc": "2.0",
  "method": "bdev_ftl_delete",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### bdev_pmem_create_pool {#rpc_bdev_pmem_create_pool}

Create a @ref bdev_config_pmem blk pool file. It is equivalent of following `pmempool create` command:

~~~bash
pmempool create -s $((num_blocks * block_size)) blk $block_size $pmem_file
~~~

This method is available only if SPDK was built with PMDK support.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
pmem_file               | Required | string      | Path to new pmem file
num_blocks              | Required | number      | Number of blocks
block_size              | Required | number      | Size of each block in bytes

#### Example

Example request:

~~~json
{
  "params": {
    "block_size": 512,
    "num_blocks": 131072,
    "pmem_file": "/tmp/pmem_file"
  },
  "jsonrpc": "2.0",
  "method": "bdev_pmem_create_pool",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### bdev_pmem_get_pool_info {#rpc_bdev_pmem_get_pool_info}

Retrieve basic information about PMDK memory pool.

This method is available only if SPDK was built with PMDK support.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
pmem_file               | Required | string      | Path to existing pmem file

#### Result

Array of objects describing memory pool:

Name                    | Type        | Description
----------------------- | ----------- | -----------
num_blocks              | number      | Number of blocks
block_size              | number      | Size of each block in bytes

#### Example

Example request:

~~~json
request:
{
  "params": {
    "pmem_file": "/tmp/pmem_file"
  },
  "jsonrpc": "2.0",
  "method": "bdev_pmem_get_pool_info",
  "id": 1
}
~~~

Example response:

~~~json
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

### bdev_pmem_delete_pool {#rpc_bdev_pmem_delete_pool}

Delete pmem pool by removing file `pmem_file`. This method will fail if `pmem_file` is not a
valid pmem pool file.

This method is available only if SPDK was built with PMDK support.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
pmem_file               | Required | string      | Path to new pmem file

#### Example

Example request:

~~~json
{
  "params": {
    "pmem_file": "/tmp/pmem_file"
  },
  "jsonrpc": "2.0",
  "method": "bdev_pmem_delete_pool",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### bdev_pmem_create {#rpc_bdev_pmem_create}

Construct @ref bdev_config_pmem bdev.

This method is available only if SPDK was built with PMDK support.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | Bdev name
pmem_file               | Required | string      | Path to existing pmem blk pool file

#### Result

Name of newly created bdev.

#### Example

Example request:

~~~json
{
  "params": {
    "pmem_file": "/tmp/pmem_file",
    "name": "Pmem0"
  },
  "jsonrpc": "2.0",
  "method": "bdev_pmem_create",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": "Pmem0"
}
~~~

### bdev_pmem_delete {#rpc_bdev_pmem_delete}

Delete @ref bdev_config_pmem bdev. This call will not remove backing pool files.

This method is available only if SPDK was built with PMDK support.

#### Result

`true` if bdev with provided name was deleted or `false` otherwise.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | Bdev name

#### Example

Example request:

~~~json
{
  "params": {
    "name": "Pmem0"
  },
  "jsonrpc": "2.0",
  "method": "bdev_pmem_delete",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### bdev_passthru_create {#rpc_bdev_passthru_create}

Create passthru bdev. This bdev type redirects all IO to it's base bdev. It has no other purpose than being an example
and a starting point in development of new bdev type.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | Bdev name
base_bdev_name          | Required | string      | Base bdev name

#### Result

Name of newly created bdev.

#### Example

Example request:

~~~json
{
  "params": {
    "base_bdev_name": "Malloc0",
    "name": "Passsthru0"
  },
  "jsonrpc": "2.0",
  "method": "bdev_passthru_create",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": "Passsthru0"
}
~~~

### bdev_passthru_delete {#rpc_bdev_passthru_delete}

Delete passthru bdev.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | Bdev name

#### Example

Example request:

~~~json
{
  "params": {
    "name": "Passsthru0"
  },
  "jsonrpc": "2.0",
  "method": "bdev_passthru_delete",
  "id": 1
}

~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### bdev_virtio_attach_controller {#rpc_bdev_virtio_attach_controller}

Create new initiator @ref bdev_config_virtio_scsi or @ref bdev_config_virtio_blk and expose all found bdevs.

#### Parameters

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

#### Result

Array of names of newly created bdevs.

#### Example

Example request:

~~~json
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
  "method": "bdev_virtio_attach_controller",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": ["VirtioScsi0t2", "VirtioScsi0t4"]
}
~~~

### bdev_virtio_scsi_get_devices {#rpc_bdev_virtio_scsi_get_devices}

Show information about all available Virtio SCSI devices.

#### Parameters

This method has no parameters.

#### Result

Array of Virtio SCSI information objects.

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "bdev_virtio_scsi_get_devices",
  "id": 1
}
~~~

Example response:

~~~json
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

### bdev_virtio_detach_controller {#rpc_bdev_virtio_detach_controller}

Remove a Virtio device. This command can be used to remove any type of virtio device.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | Virtio name

#### Example

Example request:

~~~json
{
  "params": {
    "name": "VirtioUser0"
  },
  "jsonrpc": "2.0",
  "method": "bdev_virtio_detach_controller",
  "id": 1
}

~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### bdev_virtio_blk_set_hotplug {#rpc_bdev_virtio_blk_set_hotplug}

Enable/Disable the virtio blk hotplug monitor or change the monitor period time

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
enable                  | Required | bool        | Enable or disable the virtio blk hotplug monitor
period-us               | Optional | number      | The period time of the monitor

When the enable is true then the period-us is optional. If user don't set the period time then use the default
value. When the enable is false then the period-us is not required.

#### Result

True the rpc is successful otherwise false

#### Example

Example request:

~~~json
{
  "params": {
    "enable": "true",
    "period-us": "1000000"
  },
  "jsonrpc": "2.0",
  "method": "bdev_virtio_blk_set_hotplug",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

## iSCSI Target {#jsonrpc_components_iscsi_tgt}

### iscsi_set_options method {#rpc_iscsi_set_options}

Set global parameters for iSCSI targets.

This RPC may only be called before SPDK subsystems have been initialized. This RPC can be called only once.

#### Parameters

Name                            | Optional | Type    | Description
------------------------------- | -------- | ------- | -----------
auth_file                       | Optional | string  | Path to CHAP shared secret file (default: "")
node_base                       | Optional | string  | Prefix of the name of iSCSI target node (default: "iqn.2016-06.io.spdk")
nop_timeout                     | Optional | number  | Timeout in seconds to nop-in request to the initiator (default: 60)
nop_in_interval                 | Optional | number  | Time interval in secs between nop-in requests by the target (default: 30)
disable_chap                    | Optional | boolean | CHAP for discovery session should be disabled (default: `false`)
require_chap                    | Optional | boolean | CHAP for discovery session should be required (default: `false`)
mutual_chap                     | Optional | boolean | CHAP for discovery session should be unidirectional (`false`) or bidirectional (`true`) (default: `false`)
chap_group                      | Optional | number  | CHAP group ID for discovery session (default: 0)
max_sessions                    | Optional | number  | Maximum number of sessions in the host (default: 128)
max_queue_depth                 | Optional | number  | Maximum number of outstanding I/Os per queue (default: 64)
max_connections_per_session     | Optional | number  | Session specific parameter, MaxConnections (default: 2)
default_time2wait               | Optional | number  | Session specific parameter, DefaultTime2Wait (default: 2)
default_time2retain             | Optional | number  | Session specific parameter, DefaultTime2Retain (default: 20)
first_burst_length              | Optional | number  | Session specific parameter, FirstBurstLength (default: 8192)
immediate_data                  | Optional | boolean | Session specific parameter, ImmediateData (default: `true`)
error_recovery_level            | Optional | number  | Session specific parameter, ErrorRecoveryLevel (default: 0)
allow_duplicated_isid           | Optional | boolean | Allow duplicated initiator session ID (default: `false`)
max_large_datain_per_connection | Optional | number  | Max number of outstanding split read I/Os per connection (default: 64)
max_r2t_per_connection          | Optional | number  | Max number of outstanding R2Ts per connection (default: 4)
pdu_pool_size                   | Optional | number  | Number of PDUs in the pool (default: approximately 2 * max_sessions * (max_queue_depth + max_connections_per_session))
immediate_data_pool_size        | Optional | number  | Number of immediate data buffers in the pool (default: 128 * max_sessions)
data_out_pool_size              | Optional | number  | Number of data out buffers in the pool (default: 16 * max_sessions)

To load CHAP shared secret file, its path is required to specify explicitly in the parameter `auth_file`.

Parameters `disable_chap` and `require_chap` are mutually exclusive. Parameters `no_discovery_auth`, `req_discovery_auth`,
`req_discovery_auth_mutual`, and `discovery_auth_group` are still available instead of `disable_chap`, `require_chap`,
`mutual_chap`, and `chap_group`, respectivey but will be removed in future releases.

#### Example

Example request:

~~~json
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
  "method": "iscsi_set_options",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### iscsi_get_options method {#rpc_iscsi_get_options}

Show global parameters of iSCSI targets.

#### Parameters

This method has no parameters.

#### Example

Example request:

~~~json
request:
{
  "jsonrpc": "2.0",
  "method": "iscsi_get_options",
  "id": 1
}
~~~

Example response:

~~~json
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
    "disable_chap": true,
    "default_time2wait": 2,
    "require_chap": false,
    "max_large_datain_per_connection": 64,
    "max_r2t_per_connection": 4
  }
}
~~~

### scsi_get_devices {#rpc_scsi_get_devices}

Display SCSI devices

#### Parameters

This method has no parameters.

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "scsi_get_devices",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": [
    {
      "id": 0,
      "device_name": "iqn.2016-06.io.spdk:Target3"
    }
  ]
}
~~~

### iscsi_set_discovery_auth method {#rpc_iscsi_set_discovery_auth}

Set CHAP authentication for sessions dynamically.

#### Parameters

Name                        | Optional | Type    | Description
--------------------------- | -------- | --------| -----------
disable_chap                | Optional | boolean | CHAP for discovery session should be disabled (default: `false`)
require_chap                | Optional | boolean | CHAP for discovery session should be required (default: `false`)
mutual_chap                 | Optional | boolean | CHAP for discovery session should be unidirectional (`false`) or bidirectional (`true`) (default: `false`)
chap_group                  | Optional | number  | CHAP group ID for discovery session (default: 0)

Parameters `disable_chap` and `require_chap` are mutually exclusive.

#### Example

Example request:

~~~json
request:
{
  "params": {
    "chap_group": 1,
    "require_chap": true,
    "mutual_chap": true
  },
  "jsonrpc": "2.0",
  "method": "iscsi_set_discovery_auth",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### iscsi_create_auth_group method {#rpc_iscsi_create_auth_group}

Create an authentication group for CHAP authentication.

#### Parameters

Name                        | Optional | Type    | Description
--------------------------- | -------- | --------| -----------
tag                         | Required | number  | Authentication group tag (unique, integer > 0)
secrets                     | Optional | array   | Array of @ref rpc_iscsi_create_auth_group_secret objects

#### secret {#rpc_iscsi_create_auth_group_secret}

Name                        | Optional | Type    | Description
--------------------------- | ---------| --------| -----------
user                        | Required | string  | Unidirectional CHAP name
secret                      | Required | string  | Unidirectional CHAP secret
muser                       | Optional | string  | Bidirectional CHAP name
msecret                     | Optional | string  | Bidirectional CHAP secret

#### Example

Example request:

~~~json
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
  "method": "iscsi_create_auth_group",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### iscsi_delete_auth_group method {#rpc_iscsi_delete_auth_group}

Delete an existing authentication group for CHAP authentication.

#### Parameters

Name                        | Optional | Type    | Description
--------------------------- | -------- | --------| -----------
tag                         | Required | number  | Authentication group tag (unique, integer > 0)

#### Example

Example request:

~~~json
{
  "params": {
    "tag": 2
  },
  "jsonrpc": "2.0",
  "method": "iscsi_delete_auth_group",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### iscsi_get_auth_groups {#rpc_iscsi_get_auth_groups}

Show information about all existing authentication group for CHAP authentication.

#### Parameters

This method has no parameters.

#### Result

Array of objects describing authentication group.

Name                        | Type    | Description
--------------------------- | --------| -----------
tag                         | number  | Authentication group tag
secrets                     | array   | Array of @ref rpc_iscsi_create_auth_group_secret objects

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "iscsi_get_auth_groups",
  "id": 1
}
~~~

Example response:

~~~json
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

### iscsi_auth_group_add_secret {#rpc_iscsi_auth_group_add_secret}

Add a secret to an existing authentication group for CHAP authentication.

#### Parameters

Name                        | Optional | Type    | Description
--------------------------- | -------- | --------| -----------
tag                         | Required | number  | Authentication group tag (unique, integer > 0)
user                        | Required | string  | Unidirectional CHAP name
secret                      | Required | string  | Unidirectional CHAP secret
muser                       | Optional | string  | Bidirectional CHAP name
msecret                     | Optional | string  | Bidirectional CHAP secret

#### Example

Example request:

~~~json
{
  "params": {
    "muser": "mu3",
    "secret": "s3",
    "tag": 2,
    "user": "u3",
    "msecret": "ms3"
  },
  "jsonrpc": "2.0",
  "method": "iscsi_auth_group_add_secret",
  "id": 1
}
~~~json

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### iscsi_auth_group_remove_secret {#rpc_iscsi_auth_group_remove_secret}

Remove a secret from an existing authentication group for CHAP authentication.

#### Parameters

Name                        | Optional | Type    | Description
--------------------------- | -------- | --------| -----------
tag                         | Required | number  | Authentication group tag (unique, integer > 0)
user                        | Required | string  | Unidirectional CHAP name

#### Example

Example request:

~~~json
{
  "params": {
    "tag": 2,
    "user": "u3"
  },
  "jsonrpc": "2.0",
  "method": "iscsi_auth_group_remove_secret",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### iscsi_get_initiator_groups method {#rpc_iscsi_get_initiator_groups}

Show information about all available initiator groups.

#### Parameters

This method has no parameters.

#### Result

Array of objects describing initiator groups.

Name                        | Type    | Description
--------------------------- | --------| -----------
tag                         | number  | Initiator group tag
initiators                  | array   | Array of initiator hostnames or IP addresses
netmasks                    | array   | Array of initiator netmasks

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "iscsi_get_initiator_groups",
  "id": 1
}
~~~

Example response:

~~~json
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

### iscsi_create_initiator_group method {#rpc_iscsi_create_initiator_group}

Add an initiator group.

#### Parameters

Name                        | Optional | Type    | Description
--------------------------- | -------- | --------| -----------
tag                         | Required | number  | Initiator group tag (unique, integer > 0)
initiators                  | Required | array   | Not empty array of initiator hostnames or IP addresses
netmasks                    | Required | array   | Not empty array of initiator netmasks

#### Example

Example request:

~~~json
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
  "method": "iscsi_create_initiator_group",
  "id": 1
}
~~~

Example response:

~~~json
response:
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### iscsi_delete_initiator_group method {#rpc_iscsi_delete_initiator_group}

Delete an existing initiator group.

#### Parameters

Name                        | Optional | Type    | Description
--------------------------- | -------- | --------| -----------
tag                         | Required | number  | Initiator group tag (unique, integer > 0)

#### Example

Example request:

~~~json
{
  "params": {
    "tag": 1
  },
  "jsonrpc": "2.0",
  "method": "iscsi_delete_initiator_group",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### iscsi_initiator_group_add_initiators method {#rpc_iscsi_initiator_group_add_initiators}

Add initiators to an existing initiator group.

#### Parameters

Name                        | Optional | Type    | Description
--------------------------- | -------- | --------| -----------
tag                         | Required | number  | Existing initiator group tag.
initiators                  | Optional | array   | Array of initiator hostnames or IP addresses
netmasks                    | Optional | array   | Array of initiator netmasks

#### Example

Example request:

~~~json
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
  "method": "iscsi_initiator_group_add_initiators",
  "id": 1
}
~~~

Example response:

~~~json
response:
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### iscsi_initiator_group_remove_initiators method {#rpc_iscsi_initiator_group_remove_initiators}

Remove initiators from an initiator group.

#### Parameters

Name                        | Optional | Type    | Description
--------------------------- | -------- | --------| -----------
tag                         | Required | number  | Existing initiator group tag.
initiators                  | Optional | array   | Array of initiator hostnames or IP addresses
netmasks                    | Optional | array   | Array of initiator netmasks

#### Example

Example request:

~~~json
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
  "method": "iscsi_initiator_group_remove_initiators",
  "id": 1
}
~~~

Example response:

~~~json
response:
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### iscsi_get_target_nodes method {#rpc_iscsi_get_target_nodes}

Show information about all available iSCSI target nodes.

#### Parameters

This method has no parameters.

#### Result

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

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "iscsi_get_target_nodes",
  "id": 1
}
~~~

Example response:

~~~json
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

### iscsi_create_target_node method {#rpc_iscsi_create_target_node}

Add an iSCSI target node.

#### Parameters

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

#### Example

Example request:

~~~json
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
  "method": "iscsi_create_target_node",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### iscsi_target_node_set_auth method {#rpc_iscsi_target_node_set_auth}

Set CHAP authentication to an existing iSCSI target node.

#### Parameters

Name                        | Optional | Type    | Description
--------------------------- | -------- | --------| -----------
name                        | Required | string  | Target node name (ASCII)
disable_chap                | Optional | boolean | CHAP authentication should be disabled for this target
require_chap                | Optional | boolean | CHAP authentication should be required for this target
mutual_chap                 | Optional | boolean | CHAP authentication should be bidirectional (`true`) or unidirectional (`false`)
chap_group                  | Optional | number  | Authentication group ID for this target node

Parameters `disable_chap` and `require_chap` are mutually exclusive.

#### Example

Example request:

~~~json
{
  "params": {
    "chap_group": 1,
    "require_chap": true,
    "name": "iqn.2016-06.io.spdk:target1",
    "mutual_chap": true
  },
  "jsonrpc": "2.0",
  "method": "iscsi_target_node_set_auth",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### iscsi_target_node_add_pg_ig_maps method {#rpc_iscsi_target_node_add_pg_ig_maps}

Add initiator group to portal group mappings to an existing iSCSI target node.

#### Parameters

Name                        | Optional | Type    | Description
--------------------------- | -------- | --------| -----------
name                        | Required | string  | Target node name (ASCII)
pg_ig_maps                  | Required | array   | Not empty array of initiator to portal group mappings objects

Portal to Initiator group mappings object:

Name                        | Optional | Type    | Description
--------------------------- | -------- | --------| -----------
ig_tag                      | Required | number  | Existing initiator group tag
pg_tag                      | Required | number  | Existing portal group tag

#### Example

Example request:

~~~json
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
  "method": "iscsi_target_node_add_pg_ig_maps",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### iscsi_target_node_remove_pg_ig_maps method {#rpc_iscsi_target_node_remove_pg_ig_maps}

Delete initiator group to portal group mappings from an existing iSCSI target node.

#### Parameters

Name                        | Optional | Type    | Description
--------------------------- | -------- | --------| -----------
name                        | Required | string  | Target node name (ASCII)
pg_ig_maps                  | Required | array   | Not empty array of Portal to Initiator group mappings objects

Portal to Initiator group mappings object:

Name                        | Optional | Type    | Description
--------------------------- | -------- | --------| -----------
ig_tag                      | Required | number  | Existing initiator group tag
pg_tag                      | Required | number  | Existing portal group tag

#### Example

Example request:

~~~json
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
  "method": "iscsi_target_node_remove_pg_ig_maps",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### iscsi_delete_target_node method {#rpc_iscsi_delete_target_node}

Delete an iSCSI target node.

#### Parameters

Name                        | Optional | Type    | Description
--------------------------- | -------- | --------| -----------
name                        | Required | string  | Target node name (ASCII)

#### Example

Example request:

~~~json
{
  "params": {
    "name": "iqn.2016-06.io.spdk:target1"
  },
  "jsonrpc": "2.0",
  "method": "iscsi_delete_target_node",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### iscsi_get_portal_groups method {#rpc_iscsi_get_portal_groups}

Show information about all available portal groups.

#### Parameters

This method has no parameters.

#### Example

Example request:

~~~json
request:
{
  "jsonrpc": "2.0",
  "method": "iscsi_get_portal_groups",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": [
    {
      "portals": [
        {
          "host": "127.0.0.1",
          "port": "3260"
        }
      ],
      "tag": 1,
      "private": false
    }
  ]
}
~~~

### iscsi_create_portal_group method {#rpc_iscsi_create_portal_group}

Add a portal group.

#### Parameters

Name                        | Optional | Type    | Description
--------------------------- | -------- | --------| -----------
tag                         | Required | number  | Portal group tag
portals                     | Required | array   | Not empty array of portals
private                     | Optional | boolean | When true, portals in this group are not returned by a discovery session. Used for login redirection. (default: `false`)
wait                        | Optional | boolean | When true, do not listen on portals until it is started explicitly. (default: `false`)

Portal object

Name                        | Optional | Type    | Description
--------------------------- | -------- | --------| -----------
host                        | Required | string  | Hostname or IP address
port                        | Required | string  | Port number

#### Example

Example request:

~~~json
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
  "method": "iscsi_create_portal_group",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### iscsi_start_portal_group method {#rpc_iscsi_start_portal_group}

Start listening on portals if the portal group is not started yet, or do nothing
if the portal group already started. Return a success response for both cases.

#### Parameters

Name                        | Optional | Type    | Description
--------------------------- | -------- | --------| -----------
tag                         | Required | number  | Existing portal group tag

#### Example

Example request:

~~~json
{
  "params": {
    "tag": 1
  },
  "jsonrpc": "2.0",
  "method": "iscsi_start_portal_group",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### iscsi_delete_portal_group method {#rpc_iscsi_delete_portal_group}

Delete an existing portal group.

#### Parameters

Name                        | Optional | Type    | Description
--------------------------- | -------- | --------| -----------
tag                         | Required | number  | Existing portal group tag

#### Example

Example request:

~~~json
{
  "params": {
    "tag": 1
  },
  "jsonrpc": "2.0",
  "method": "iscsi_delete_portal_group",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### iscsi_portal_group_set_auth method {#rpc_iscsi_portal_group_set_auth}

Set CHAP authentication for discovery sessions specific for the existing iSCSI portal group.
This RPC overwrites the setting by the global parameters for the iSCSI portal group.

#### Parameters

Name                        | Optional | Type    | Description
--------------------------- | -------- | --------| -----------
disable_chap                | Optional | boolean | CHAP for discovery session should be disabled (default: `false`)
require_chap                | Optional | boolean | CHAP for discovery session should be required (default: `false`)
mutual_chap                 | Optional | boolean | CHAP for discovery session should be unidirectional (`false`) or bidirectional (`true`) (default: `false`)
chap_group                  | Optional | number  | CHAP group ID for discovery session (default: 0)

Parameters `disable_chap` and `require_chap` are mutually exclusive.

#### Example

Example request:

~~~json
request:
{
  "params": {
    "tag": 1,
    "chap_group": 1,
    "require_chap": true,
    "mutual_chap": true
  },
  "jsonrpc": "2.0",
  "method": "iscsi_portal_group_set_auth",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### iscsi_get_connections method {#rpc_iscsi_get_connections}

Show information about all active connections.

#### Parameters

This method has no parameters.

#### Results

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

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "iscsi_get_connections",
  "id": 1
}
~~~

Example response:

~~~json
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

### iscsi_target_node_add_lun method {#rpc_iscsi_target_node_add_lun}

Add an LUN to an existing iSCSI target node.

#### Parameters

Name                        | Optional | Type    | Description
--------------------------- | -------- | --------| -----------
name                        | Required | string  | Target node name (ASCII)
bdev_name                   | Required | string  | bdev name to be added as a LUN
lun_id                      | Optional | number  | LUN ID (default: first free ID)

#### Example

Example request:

~~~json
{
  "params": {
    "lun_id": 2,
    "name": "iqn.2016-06.io.spdk:target1",
    "bdev_name": "Malloc0"
  },
  "jsonrpc": "2.0",
  "method": "iscsi_target_node_add_lun",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### iscsi_target_node_set_redirect method {#rpc_iscsi_target_node_set_redirect}

Update redirect portal of the primary portal group for the target node,

#### Parameters

Name                        | Optional | Type    | Description
--------------------------- | -------- | --------| -----------
name                        | Required | string  | Target node name (ASCII)
pg_tag                      | Required | number  | Existing portal group tag
redirect_host               | Optional | string  | Numeric IP address to which the target node is redirected
redirect_port               | Optional | string  | Numeric TCP port to which the target node is redirected

If both redirect_host and redirect_port are omitted, clear the redirect portal.

#### Example

Example request:

~~~json
{
  "params": {
    "name": "iqn.2016-06.io.spdk:target1",
    "pg_tag": 1,
    "redirect_host": "10.0.0.3",
    "redirect_port": "3260"
  },
  "jsonrpc": "2.0",
  "method": "iscsi_target_node_set_redirect",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### iscsi_target_node_request_logout method {#rpc_iscsi_target_node_request_logout}

For the target node, request connections whose portal group tag match to logout,
or request all connections to logout if portal group tag is omitted.

#### Parameters

Name                        | Optional | Type    | Description
--------------------------- | -------- | --------| -----------
name                        | Required | string  | Target node name (ASCII)
pg_tag                      | Optional | number  | Existing portal group tag

#### Example

Example request:

~~~json
{
  "params": {
    "name": "iqn.2016-06.io.spdk:target1",
    "pg_tag": 1
  },
  "jsonrpc": "2.0",
  "method": "iscsi_target_node_request_logout",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

## NVMe-oF Target {#jsonrpc_components_nvmf_tgt}

### nvmf_create_transport method {#rpc_nvmf_create_transport}

Initialize an NVMe-oF transport with the given options.

#### Parameters

Name                        | Optional | Type    | Description
--------------------------- | -------- | --------| -----------
trtype                      | Required | string  | Transport type (ex. RDMA)
tgt_name                    | Optional | string  | Parent NVMe-oF target name.
max_queue_depth             | Optional | number  | Max number of outstanding I/O per queue
max_qpairs_per_ctrlr        | Optional | number  | Max number of SQ and CQ per controller (deprecated, use max_io_qpairs_per_ctrlr)
max_io_qpairs_per_ctrlr     | Optional | number  | Max number of IO qpairs per controller
in_capsule_data_size        | Optional | number  | Max number of in-capsule data size
max_io_size                 | Optional | number  | Max I/O size (bytes)
io_unit_size                | Optional | number  | I/O unit size (bytes)
max_aq_depth                | Optional | number  | Max number of admin cmds per AQ
num_shared_buffers          | Optional | number  | The number of pooled data buffers available to the transport
buf_cache_size              | Optional | number  | The number of shared buffers to reserve for each poll group
num_cqe                     | Optional | number  | The number of CQ entires. Only used when no_srq=true (RDMA only)
max_srq_depth               | Optional | number  | The number of elements in a per-thread shared receive queue (RDMA only)
no_srq                      | Optional | boolean | Disable shared receive queue even for devices that support it. (RDMA only)
c2h_success                 | Optional | boolean | Disable C2H success optimization (TCP only)
dif_insert_or_strip         | Optional | boolean | Enable DIF insert for write I/O and DIF strip for read I/O DIF
sock_priority               | Optional | number  | The socket priority of the connection owned by this transport (TCP only)
acceptor_backlog            | Optional | number  | The number of pending connections allowed in backlog before failing new connection attempts (RDMA only)
abort_timeout_sec           | Optional | number  | Abort execution timeout value, in seconds
no_wr_batching              | Optional | boolean | Disable work requests batching (RDMA only)
control_msg_num             | Optional | number  | The number of control messages per poll group (TCP only)
disable_mappable_bar0       | Optional | boolean | disable client mmap() of BAR0 (VFIO-USER only)
zcopy                       | Optional | boolean | Use zero-copy operations if the underlying bdev supports them

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "nvmf_create_transport",
  "id": 1,
  "params": {
    "trtype": "RDMA",
    "max_queue_depth": 32
  }
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### nvmf_get_subsystems method {#rpc_nvmf_get_subsystems}

#### Parameters

Name                        | Optional | Type        | Description
--------------------------- | -------- | ------------| -----------
tgt_name                    | Optional | string      | Parent NVMe-oF target name.

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "nvmf_get_subsystems"
}
~~~

Example response:

~~~json
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
      "model_number": "ghijklmnop",
      "namespaces": [
        {"nsid": 1, "name": "Malloc2"},
        {"nsid": 2, "name": "Nvme0n1"}
      ]
    }
  ]
}
~~~

### nvmf_create_subsystem method {#rpc_nvmf_create_subsystem}

Construct an NVMe over Fabrics target subsystem.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
nqn                     | Required | string      | Subsystem NQN
tgt_name                | Optional | string      | Parent NVMe-oF target name.
serial_number           | Optional | string      | Serial number of virtual controller
model_number            | Optional | string      | Model number of virtual controller
max_namespaces          | Optional | number      | Maximum number of namespaces that can be attached to the subsystem. Default: 0 (Unlimited)
allow_any_host          | Optional | boolean     | Allow any host (`true`) or enforce allowed host list (`false`). Default: `false`.
ana_reporting           | Optional | boolean     | Enable ANA reporting feature (default: `false`).
min_cntlid              | Optional | number      | Minimum controller ID. Default: 1
max_cntlid              | Optional | number      | Maximum controller ID. Default: 0xffef

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "nvmf_create_subsystem",
  "params": {
    "nqn": "nqn.2016-06.io.spdk:cnode1",
    "allow_any_host": false,
    "serial_number": "abcdef",
    "model_number": "ghijklmnop"
  }
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### nvmf_delete_subsystem method {#rpc_nvmf_delete_subsystem}

Delete an existing NVMe-oF subsystem.

#### Parameters

Parameter              | Optional | Type        | Description
---------------------- | -------- | ----------- | -----------
nqn                    | Required | string      | Subsystem NQN to delete.
tgt_name               | Optional | string      | Parent NVMe-oF target name.

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "nvmf_delete_subsystem",
  "params": {
    "nqn": "nqn.2016-06.io.spdk:cnode1"
  }
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### nvmf_subsystem_add_listener  method {#rpc_nvmf_subsystem_add_listener}

Add a new listen address to an NVMe-oF subsystem.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
nqn                     | Required | string      | Subsystem NQN
tgt_name                | Optional | string      | Parent NVMe-oF target name.
listen_address          | Required | object      | @ref rpc_nvmf_listen_address object

#### listen_address {#rpc_nvmf_listen_address}

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
trtype                  | Required | string      | Transport type ("RDMA")
adrfam                  | Required | string      | Address family ("IPv4", "IPv6", "IB", or "FC")
traddr                  | Required | string      | Transport address
trsvcid                 | Optional | string      | Transport service ID (required for RDMA or TCP)

#### Example

Example request:

~~~json
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
      "trsvcid": "4420"
    }
  }
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### nvmf_subsystem_remove_listener  method {#rpc_nvmf_subsystem_remove_listener}

Remove a listen address from an NVMe-oF subsystem.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
nqn                     | Required | string      | Subsystem NQN
tgt_name                | Optional | string      | Parent NVMe-oF target name.
listen_address          | Required | object      | @ref rpc_nvmf_listen_address object

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "nvmf_subsystem_remove_listener",
  "params": {
    "nqn": "nqn.2016-06.io.spdk:cnode1",
    "listen_address": {
      "trtype": "RDMA",
      "adrfam": "IPv4",
      "traddr": "192.168.0.123",
      "trsvcid": "4420"
    }
  }
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### nvmf_subsystem_listener_set_ana_state  method {#rpc_nvmf_subsystem_listener_set_ana_state}

Set ANA state of a listener for an NVMe-oF subsystem. Only the ANA state of the specified ANA
group is updated, or ANA states of all ANA groups if ANA group is not specified.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
nqn                     | Required | string      | Subsystem NQN
tgt_name                | Optional | string      | Parent NVMe-oF target name.
listen_address          | Required | object      | @ref rpc_nvmf_listen_address object
ana_state               | Required | string      | ANA state to set ("optimized", "non_optimized", or "inaccessible")
anagrpid                | Optional | number      | ANA group ID

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "nvmf_subsystem_listener_set_ana_state",
  "params": {
    "nqn": "nqn.2016-06.io.spdk:cnode1",
    "listen_address": {
      "trtype": "RDMA",
      "adrfam": "IPv4",
      "traddr": "192.168.0.123",
      "trsvcid": "4420"
    },
    "ana_state", "inaccessible"
  }
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### nvmf_subsystem_add_ns method {#rpc_nvmf_subsystem_add_ns}

Add a namespace to a subsystem. The namespace ID is returned as the result.

### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
nqn                     | Required | string      | Subsystem NQN
namespace               | Required | object      | @ref rpc_nvmf_namespace object
tgt_name                | Optional | string      | Parent NVMe-oF target name.

#### namespace {#rpc_nvmf_namespace}

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
nsid                    | Optional | number      | Namespace ID between 1 and 4294967294, inclusive. Default: Automatically assign NSID.
bdev_name               | Required | string      | Name of bdev to expose as a namespace.
nguid                   | Optional | string      | 16-byte namespace globally unique identifier in hexadecimal (e.g. "ABCDEF0123456789ABCDEF0123456789")
eui64                   | Optional | string      | 8-byte namespace EUI-64 in hexadecimal (e.g. "ABCDEF0123456789")
uuid                    | Optional | string      | RFC 4122 UUID (e.g. "ceccf520-691e-4b46-9546-34af789907c5")
ptpl_file               | Optional | string      | File path to save/restore persistent reservation information
anagrpid                | Optional | number      | ANA group ID. Default: Namespace ID.

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "nvmf_subsystem_add_ns",
  "params": {
    "nqn": "nqn.2016-06.io.spdk:cnode1",
    "namespace": {
      "nsid": 3,
      "bdev_name": "Nvme0n1",
      "ptpl_file": "/opt/Nvme0n1PR.json"
    }
  }
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": 3
}
~~~

### nvmf_subsystem_remove_ns method {#rpc_nvmf_subsystem_remove_ns}

Remove a namespace from a subsystem.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
nqn                     | Required | string      | Subsystem NQN
nsid                    | Required | number      | Namespace ID
tgt_name                | Optional | string      | Parent NVMe-oF target name.

#### Example

Example request:

~~~json
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

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### nvmf_subsystem_add_host method {#rpc_nvmf_subsystem_add_host}

Add a host NQN to the list of allowed hosts.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
nqn                     | Required | string      | Subsystem NQN
host                    | Required | string      | Host NQN to add to the list of allowed host NQNs
tgt_name                | Optional | string      | Parent NVMe-oF target name.

#### Example

Example request:

~~~json
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

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### nvmf_subsystem_remove_host method {#rpc_nvmf_subsystem_remove_host}

Remove a host NQN from the list of allowed hosts.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
nqn                     | Required | string      | Subsystem NQN
host                    | Required | string      | Host NQN to remove from the list of allowed host NQNs
tgt_name                | Optional | string      | Parent NVMe-oF target name.

#### Example

Example request:

~~~json
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

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### nvmf_subsystem_allow_any_host method {#rpc_nvmf_subsystem_allow_any_host}

Configure a subsystem to allow any host to connect or to enforce the host NQN list.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
nqn                     | Required | string      | Subsystem NQN
allow_any_host          | Required | boolean     | Allow any host (`true`) or enforce allowed host list (`false`).
tgt_name                | Optional | string      | Parent NVMe-oF target name.

#### Example

Example request:

~~~json
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

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### nvmf_subsystem_get_controllers {#rpc_nvmf_subsystem_get_controllers}

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
nqn                     | Required | string      | Subsystem NQN
tgt_name                | Optional | string      | Parent NVMe-oF target name.

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "nvmf_subsystem_get_controllers",
  "params": {
    "nqn": "nqn.2016-06.io.spdk:cnode1"
  }
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": [
    {
      "cntlid": 1,
      "hostnqn": "nqn.2016-06.io.spdk:host1",
      "hostid": "27dad528-6368-41c3-82d3-0b956b49025d",
      "num_io_qpairs": 5
    }
  ]
}
~~~

### nvmf_subsystem_get_qpairs {#rpc_nvmf_subsystem_get_qpairs}

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
nqn                     | Required | string      | Subsystem NQN
tgt_name                | Optional | string      | Parent NVMe-oF target name.

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "nvmf_subsystem_get_qpairs",
  "params": {
    "nqn": "nqn.2016-06.io.spdk:cnode1"
  }
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": [
    {
      "cntlid": 1,
      "qid": 0,
      "state": "active",
      "listen_address": {
        "trtype": "RDMA",
        "adrfam": "IPv4",
        "traddr": "192.168.0.123",
        "trsvcid": "4420"
      }
    },
    {
      "cntlid": 1,
      "qid": 1,
      "state": "active",
      "listen_address": {
        "trtype": "RDMA",
        "adrfam": "IPv4",
        "traddr": "192.168.0.123",
        "trsvcid": "4420"
      }
    }
  ]
}
~~~

### nvmf_subsystem_get_listeners {#rpc_nvmf_subsystem_get_listeners}

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
nqn                     | Required | string      | Subsystem NQN
tgt_name                | Optional | string      | Parent NVMe-oF target name.

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "nvmf_subsystem_get_listeners",
  "params": {
    "nqn": "nqn.2016-06.io.spdk:cnode1"
  }
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": [
    {
      "address": {
        "trtype": "RDMA",
        "adrfam": "IPv4",
        "traddr": "192.168.0.123",
        "trsvcid": "4420"
      },
      "ana_state": "optimized"
    }
  ]
}
~~~

### nvmf_set_max_subsystems {#rpc_nvmf_set_max_subsystems}

Set the maximum allowed subsystems for the NVMe-oF target.  This RPC may only be called
before SPDK subsystems have been initialized.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
max_subsystems          | Required | number      | Maximum number of NVMe-oF subsystems

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "nvmf_set_max_subsystems",
  "params": {
    "max_subsystems": 1024
  }
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### nvmf_set_config {#rpc_nvmf_set_config}

Set global configuration of NVMe-oF target.  This RPC may only be called before SPDK subsystems
have been initialized.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
acceptor_poll_rate      | Optional | number      | Polling interval of the acceptor for incoming connections (microseconds)
admin_cmd_passthru      | Optional | object      | Admin command passthru configuration
poll_groups_mask        | Optional | string      | Set cpumask for NVMf poll groups
discovery_filter        | Optional | string      | Set discovery filter, possible values are: `match_any` (default) or comma separated values: `transport`, `address`, `svcid`

#### admin_cmd_passthru {#spdk_nvmf_admin_passthru_conf}

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
identify_ctrlr          | Required | bool        | If true, enables custom identify handler that reports some identify attributes from the underlying NVMe drive

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "nvmf_set_config",
  "params": {
    "acceptor_poll_rate": 10000
  }
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### nvmf_get_transports method {#rpc_nvmf_get_transports}

#### Parameters

The user may specify no parameters in order to list all transports, or a transport may be
specified by type.

Name                        | Optional | Type        | Description
--------------------------- | -------- | ------------| -----------
tgt_name                    | Optional | string      | Parent NVMe-oF target name.
trtype                      | Optional | string      | Transport type.

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "nvmf_get_transports"
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": [
    {
      "type": "RDMA".
      "max_queue_depth": 128,
      "max_io_qpairs_per_ctrlr": 64,
      "in_capsule_data_size": 4096,
      "max_io_size": 131072,
      "io_unit_size": 131072,
      "abort_timeout_sec": 1
    }
  ]
}
~~~

### nvmf_get_stats method {#rpc_nvmf_get_stats}

Retrieve current statistics of the NVMf subsystem.

#### Parameters

Name                        | Optional | Type        | Description
--------------------------- | -------- | ------------| -----------
tgt_name                    | Optional | string      | Parent NVMe-oF target name.

#### Response

The response is an object containing NVMf subsystem statistics.
In the response, `admin_qpairs` and `io_qpairs` are reflecting cumulative queue pair counts while
`current_admin_qpairs` and `current_io_qpairs` are showing the current number.

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "nvmf_get_stats",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "tick_rate": 2400000000,
    "poll_groups": [
      {
        "name": "app_thread",
        "admin_qpairs": 1,
        "io_qpairs": 4,
        "current_admin_qpairs": 1,
        "current_io_qpairs": 2,
        "pending_bdev_io": 1721,
        "transports": [
          {
            "trtype": "RDMA",
            "pending_data_buffer": 12131888,
            "devices": [
              {
                "name": "mlx5_1",
                "polls": 72284105,
                "completions": 0,
                "requests": 0,
                "request_latency": 0,
                "pending_free_request": 0,
                "pending_rdma_read": 0,
                "pending_rdma_write": 0,
                "total_send_wrs": 0,
                "send_doorbell_updates": 0,
                "total_recv_wrs": 0,
                "recv_doorbell_updates": 1
              },
              {
                "name": "mlx5_0",
                "polls": 72284105,
                "completions": 15165875,
                "requests": 7582935,
                "request_latency": 1249323766184,
                "pending_free_request": 0,
                "pending_rdma_read": 337602,
                "pending_rdma_write": 0,
                "total_send_wrs": 15165875,
                "send_doorbell_updates": 1516587,
                "total_recv_wrs": 15165875,
                "recv_doorbell_updates": 1516587
              }
            ]
          }
        ]
      }
    ]
  }
}
~~~

### nvmf_set_crdt {#rpc_nvmf_set_crdt}

Set the 3 CRDT (Command Retry Delay Time) values. For details about
CRDT, please refer to the NVMe specification. Currently all the
SPDK nvmf subsystems share the same CRDT values. The default values
are 0. This rpc can only be invoked in STARTUP stage. All values are
in units of 100 milliseconds (same as the NVMe specification).

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
crdt1                   | Optional | number      | Command Retry Delay Time 1
crdt2                   | Optional | number      | Command Retry Delay Time 2
crdt3                   | Optional | number      | Command Retry Delay Time 3

## Vhost Target {#jsonrpc_components_vhost_tgt}

The following common preconditions need to be met in all target types.

Controller name will be used to create UNIX domain socket. This implies that name concatenated with vhost socket
directory path needs to be valid UNIX socket name.

@ref cpu_mask parameter is used to choose CPU on which pollers will be launched when new initiator is connecting.
It must be a subset of application CPU mask. Default value is CPU mask of the application.

### vhost_controller_set_coalescing {#rpc_vhost_controller_set_coalescing}

Controls interrupt coalescing for specific target. Because `delay_base_us` is used to calculate delay in CPU ticks
there is no hardcoded limit for this parameter. Only limitation is that final delay in CPU ticks might not overflow
32 bit unsigned integer (which is more than 1s @ 4GHz CPU). In real scenarios `delay_base_us` should be much lower
than 150us. To disable coalescing set `delay_base_us` to 0.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
ctrlr                   | Required | string      | Controller name
delay_base_us           | Required | number      | Base (minimum) coalescing time in microseconds
iops_threshold          | Required | number      | Coalescing activation level greater than 0 in IO per second

#### Example

Example request:

~~~json
{
  "params": {
    "iops_threshold": 100000,
    "ctrlr": "VhostScsi0",
    "delay_base_us": 80
  },
  "jsonrpc": "2.0",
  "method": "vhost_controller_set_coalescing",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### vhost_create_scsi_controller {#rpc_vhost_create_scsi_controller}

Construct vhost SCSI target.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
ctrlr                   | Required | string      | Controller name
cpumask                 | Optional | string      | @ref cpu_mask for this controller

#### Example

Example request:

~~~json
{
  "params": {
    "cpumask": "0x2",
    "ctrlr": "VhostScsi0"
  },
  "jsonrpc": "2.0",
  "method": "vhost_create_scsi_controller",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### vhost_scsi_controller_add_target {#rpc_vhost_scsi_controller_add_target}

In vhost target `ctrlr` create SCSI target with ID `scsi_target_num` and add `bdev_name` as LUN 0.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
ctrlr                   | Required | string      | Controller name
scsi_target_num         | Required | number      | SCSI target ID between 0 and 7 or -1 to use first free ID.
bdev_name               | Required | string      | Name of bdev to expose as a LUN 0

#### Response

SCSI target ID.

#### Example

Example request:

~~~json
{
  "params": {
    "scsi_target_num": 1,
    "bdev_name": "Malloc0",
    "ctrlr": "VhostScsi0"
  },
  "jsonrpc": "2.0",
  "method": "vhost_scsi_controller_add_target",
  "id": 1
}
~~~

Example response:

~~~json
response:
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": 1
}
~~~

### vhost_scsi_controller_remove_target {#rpc_vhost_scsi_controller_remove_target}

Remove SCSI target ID `scsi_target_num` from vhost target `scsi_target_num`.

This method will fail if initiator is connected, but doesn't support hot-remove (the `VIRTIO_SCSI_F_HOTPLUG` is not negotiated).

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
ctrlr                   | Required | string      | Controller name
scsi_target_num         | Required | number      | SCSI target ID between 0 and 7

#### Example

Example request:

~~~json
request:
{
  "params": {
    "scsi_target_num": 1,
    "ctrlr": "VhostScsi0"
  },
  "jsonrpc": "2.0",
  "method": "vhost_scsi_controller_remove_target",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### vhost_create_blk_controller {#rpc_vhost_create_blk_controller}

Create vhost block controller

If `readonly` is `true` then vhost block target will be created as read only and fail any write requests.
The `VIRTIO_BLK_F_RO` feature flag will be offered to the initiator.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
ctrlr                   | Required | string      | Controller name
bdev_name               | Required | string      | Name of bdev to expose block device
readonly                | Optional | boolean     | If true, this target will be read only (default: false)
cpumask                 | Optional | string      | @ref cpu_mask for this controller

#### Example

Example request:

~~~json
{
  "params": {
    "dev_name": "Malloc0",
    "ctrlr": "VhostBlk0"
  },
  "jsonrpc": "2.0",
  "method": "vhost_create_blk_controller",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### vhost_get_controllers {#rpc_vhost_get_controllers}

Display information about all or specific vhost controller(s).

#### Parameters

The user may specify no parameters in order to list all controllers, or a controller may be
specified by name.

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Optional | string      | Vhost controller name

#### Response {#rpc_vhost_get_controllers_response}

Response is an array of objects describing requested controller(s). Common fields are:

Name                    | Type        | Description
----------------------- | ----------- | -----------
ctrlr                   | string      | Controller name
cpumask                 | string      | @ref cpu_mask of this controller
delay_base_us           | number      | Base (minimum) coalescing time in microseconds (0 if disabled)
iops_threshold          | number      | Coalescing activation level
backend_specific        | object      | Backend specific informations

### Vhost block {#rpc_vhost_get_controllers_blk}

`backend_specific` contains one `block` object  of type:

Name                    | Type        | Description
----------------------- | ----------- | -----------
bdev                    | string      | Backing bdev name or Null if bdev is hot-removed
readonly                | boolean     | True if controllers is readonly, false otherwise

### Vhost SCSI {#rpc_vhost_get_controllers_scsi}

`backend_specific` contains `scsi` array of following objects:

Name                    | Type        | Description
----------------------- | ----------- | -----------
target_name             | string      | Name of this SCSI target
id                      | number      | Unique SPDK global SCSI target ID
scsi_dev_num            | number      | SCSI target ID initiator will see when scanning this controller
luns                    | array       | array of objects describing @ref rpc_vhost_get_controllers_scsi_luns

### Vhost SCSI LUN {#rpc_vhost_get_controllers_scsi_luns}

Object of type:

Name                    | Type        | Description
----------------------- | ----------- | -----------
id                      | number      | SCSI LUN ID
bdev_name               | string      | Backing bdev name

### Vhost NVMe {#rpc_vhost_get_controllers_nvme}

`backend_specific` contains `namespaces` array of following objects:

Name                    | Type        | Description
----------------------- | ----------- | -----------
nsid                    | number      | Namespace ID
bdev                    | string      | Backing bdev name

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "vhost_get_controllers",
  "id": 1
}
~~~

Example response:

~~~json
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

### vhost_delete_controller {#rpc_vhost_delete_controller}

Remove vhost target.

This call will fail if there is an initiator connected or there is at least one SCSI target configured in case of
vhost SCSI target. In the later case please remove all SCSI targets first using @ref rpc_vhost_scsi_controller_remove_target.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
ctrlr                   | Required | string      | Controller name

#### Example

Example request:

~~~json
{
  "params": {
    "ctrlr": "VhostNvme0"
  },
  "jsonrpc": "2.0",
  "method": "vhost_delete_controller",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

## Logical Volume {#jsonrpc_components_lvol}

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

### bdev_lvol_create_lvstore {#rpc_bdev_lvol_create_lvstore}

Construct a logical volume store.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
bdev_name               | Required | string      | Bdev on which to construct logical volume store
lvs_name                | Required | string      | Name of the logical volume store to create
cluster_sz              | Optional | number      | Cluster size of the logical volume store in bytes
clear_method            | Optional | string      | Change clear method for data region. Available: none, unmap (default), write_zeroes

#### Response

UUID of the created logical volume store is returned.

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "bdev_lvol_create_lvstore",
  "params": {
    "lvs_name": "LVS0",
    "bdev_name": "Malloc0"
    "clear_method": "write_zeroes"
  }
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": "a9959197-b5e2-4f2d-8095-251ffb6985a5"
}
~~~

### bdev_lvol_delete_lvstore {#rpc_bdev_lvol_delete_lvstore}

Destroy a logical volume store.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
uuid                    | Optional | string      | UUID of the logical volume store to destroy
lvs_name                | Optional | string      | Name of the logical volume store to destroy

Either uuid or lvs_name must be specified, but not both.

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "bdev_lvol_delete_lvstore",
  "id": 1
  "params": {
    "uuid": "a9959197-b5e2-4f2d-8095-251ffb6985a5"
  }
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### bdev_lvol_get_lvstores {#rpc_bdev_lvol_get_lvstores}

Get a list of logical volume stores.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
uuid                    | Optional | string      | UUID of the logical volume store to retrieve information about
lvs_name                | Optional | string      | Name of the logical volume store to retrieve information about

Either uuid or lvs_name may be specified, but not both.
If both uuid and lvs_name are omitted, information about all logical volume stores is returned.

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "bdev_lvol_get_lvstores",
  "id": 1,
  "params": {
    "lvs_name": "LVS0"
  }
}
~~~

Example response:

~~~json
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

### bdev_lvol_rename_lvstore {#rpc_bdev_lvol_rename_lvstore}

Rename a logical volume store.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
old_name                | Required | string      | Existing logical volume store name
new_name                | Required | string      | New logical volume store name

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "bdev_lvol_rename_lvstore",
  "id": 1,
  "params": {
    "old_name": "LVS0",
    "new_name": "LVS2"
  }
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### bdev_lvol_create {#rpc_bdev_lvol_create}

Create a logical volume on a logical volume store.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
lvol_name               | Required | string      | Name of logical volume to create
size                    | Required | number      | Desired size of logical volume in bytes
thin_provision          | Optional | boolean     | True to enable thin provisioning
uuid                    | Optional | string      | UUID of logical volume store to create logical volume on
lvs_name                | Optional | string      | Name of logical volume store to create logical volume on
clear_method            | Optional | string      | Change default data clusters clear method. Available: none, unmap, write_zeroes

Size will be rounded up to a multiple of cluster size. Either uuid or lvs_name must be specified, but not both.
lvol_name will be used in the alias of the created logical volume.

#### Response

UUID of the created logical volume is returned.

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "bdev_lvol_create",
  "id": 1,
  "params": {
    "lvol_name": "LVOL0",
    "size": 1048576,
    "lvs_name": "LVS0",
    "clear_method": "unmap",
    "thin_provision": true
  }
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": "1b38702c-7f0c-411e-a962-92c6a5a8a602"
}
~~~

### bdev_lvol_snapshot {#rpc_bdev_lvol_snapshot}

Capture a snapshot of the current state of a logical volume.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
lvol_name               | Required | string      | UUID or alias of the logical volume to create a snapshot from
snapshot_name           | Required | string      | Name for the newly created snapshot

#### Response

UUID of the created logical volume snapshot is returned.

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "bdev_lvol_snapshot",
  "id": 1,
  "params": {
    "lvol_name": "1b38702c-7f0c-411e-a962-92c6a5a8a602",
    "snapshot_name": "SNAP1"
  }
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": "cc8d7fdf-7865-4d1f-9fc6-35da8e368670"
}
~~~

### bdev_lvol_clone {#rpc_bdev_lvol_clone}

Create a logical volume based on a snapshot.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
snapshot_name           | Required | string      | UUID or alias of the snapshot to clone
clone_name              | Required | string      | Name for the logical volume to create

#### Response

UUID of the created logical volume clone is returned.

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0"
  "method": "bdev_lvol_clone",
  "id": 1,
  "params": {
    "snapshot_name": "cc8d7fdf-7865-4d1f-9fc6-35da8e368670",
    "clone_name": "CLONE1"
  }
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": "8d87fccc-c278-49f0-9d4c-6237951aca09"
}
~~~

### bdev_lvol_rename {#rpc_bdev_lvol_rename}

Rename a logical volume. New name will rename only the alias of the logical volume.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
old_name                | Required | string      | UUID or alias of the existing logical volume
new_name                | Required | string      | New logical volume name

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "bdev_lvol_rename",
  "id": 1,
  "params": {
    "old_name": "067df606-6dbc-4143-a499-0d05855cb3b8",
    "new_name": "LVOL2"
  }
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### bdev_lvol_resize {#rpc_bdev_lvol_resize}

Resize a logical volume.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | UUID or alias of the logical volume to resize
size                    | Required | number      | Desired size of the logical volume in bytes

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "bdev_lvol_resize",
  "id": 1,
  "params": {
    "name": "51638754-ca16-43a7-9f8f-294a0805ab0a",
    "size": 2097152
  }
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### bdev_lvol_set_read_only{#rpc_bdev_lvol_set_read_only}

Mark logical volume as read only.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | UUID or alias of the logical volume to set as read only

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "bdev_lvol_set_read_only",
  "id": 1,
  "params": {
    "name": "51638754-ca16-43a7-9f8f-294a0805ab0a",
  }
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### bdev_lvol_delete {#rpc_bdev_lvol_delete}

Destroy a logical volume.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | UUID or alias of the logical volume to destroy

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "bdev_lvol_delete",
  "id": 1,
  "params": {
    "name": "51638754-ca16-43a7-9f8f-294a0805ab0a"
  }
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### bdev_lvol_inflate {#rpc_bdev_lvol_inflate}

Inflate a logical volume. All unallocated clusters are allocated and copied from the parent or zero filled
if not allocated in the parent. Then all dependencies on the parent are removed.

### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | UUID or alias of the logical volume to inflate

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "bdev_lvol_inflate",
  "id": 1,
  "params": {
    "name": "8d87fccc-c278-49f0-9d4c-6237951aca09"
  }
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### bdev_lvol_decouple_parent {#rpc_bdev_lvol_decouple_parent}

Decouple the parent of a logical volume. For unallocated clusters which is allocated in the parent, they are
allocated and copied from the parent, but for unallocated clusters which is thin provisioned in the parent,
they are kept thin provisioned. Then all dependencies on the parent are removed.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | UUID or alias of the logical volume to decouple the parent of it

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "bdev_lvol_decouple_parent",
  "id": 1.
  "params": {
    "name": "8d87fccc-c278-49f0-9d4c-6237951aca09"
  }
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

## RAID

### bdev_raid_get_bdevs {#rpc_bdev_raid_get_bdevs}

This is used to list all the raid bdev names based on the input category requested. Category should be one
of 'all', 'online', 'configuring' or 'offline'. 'all' means all the raid bdevs whether they are online or
configuring or offline. 'online' is the raid bdev which is registered with bdev layer. 'configuring' is
the raid bdev which does not have full configuration discovered yet. 'offline' is the raid bdev which is
not registered with bdev as of now and it has encountered any error or user has requested to offline
the raid bdev.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
category                | Required | string      | all or online or configuring or offline

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "bdev_raid_get_bdevs",
  "id": 1,
  "params": {
    "category": "all"
  }
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": [
    "Raid0"
  ]
}
~~~

### bdev_raid_create {#rpc_bdev_raid_create}

Constructs new RAID bdev.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | RAID bdev name
strip_size_kb           | Required | number      | Strip size in KB
raid_level              | Required | string      | RAID level
base_bdevs              | Required | string      | Base bdevs name, whitespace separated list in quotes

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "bdev_raid_create",
  "id": 1,
  "params": {
    "name": "Raid0",
    "raid_level": "0",
    "base_bdevs": [
      "Malloc0",
      "Malloc1",
      "Malloc2",
      "Malloc3"
    ],
    "strip_size_kb": 4
  }
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### bdev_raid_delete {#rpc_bdev_raid_delete}

Removes RAID bdev.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | RAID bdev name

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "bdev_raid_delete",
  "id": 1,
  "params": {
    "name": "Raid0"
  }
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

## SPLIT

### bdev_split_create {#rpc_bdev_split_create}

This is used to split an underlying block device and create several smaller equal-sized vbdevs.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
base_bdev               | Required | string      | base bdev name
split_count             | Required | number      | number of splits
split_size_mb           | Optional | number      | size in MB to restrict the size

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "bdev_split_create",
  "id": 1,
  "params": {
    "base_bdev": "Malloc0",
    "split_count": 4
  }
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": [
    "Malloc0p0",
    "Malloc0p1",
    "Malloc0p2",
    "Malloc0p3"
  ]
}
~~~

### bdev_split_delete {#rpc_bdev_split_delete}

This is used to remove the split vbdevs.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
base_bdev               | Required | string      | base bdev name

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "bdev_split_delete",
  "id": 1,
  "params": {
    "base_bdev": "Malloc0"
  }
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

## Uring

### bdev_uring_create {#rpc_bdev_uring_create}

Create a bdev with io_uring backend.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
filename                | Required | string      | path to device or file (ex: /dev/nvme0n1)
name                    | Required | string      | name of bdev
block_size              | Optional | number      | block size of device (If omitted, get the block size from the file)

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "bdev_uring_create",
  "id": 1,
  "params": {
    "name": "bdev_u0",
    "filename": "/dev/nvme0n1",
    "block_size": 512
  }
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": "bdev_u0"
}
~~~

### bdev_uring_delete {#rpc_bdev_uring_delete}

Remove a uring bdev.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | name of uring bdev to delete

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "bdev_uring_delete",
  "id": 1,
  "params": {
    "name": "bdev_u0"
  }
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

## OPAL

### bdev_nvme_opal_init {#rpc_bdev_nvme_opal_init}

This is used to initialize OPAL of a given NVMe ctrlr, including taking ownership and activating.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
nvme_ctrlr_name         | Required | string      | name of nvme ctrlr
password                | Required | string      | admin password of OPAL

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "bdev_nvme_opal_init",
  "id": 1,
  "params": {
    "nvme_ctrlr_name": "nvme0",
    "password": "*****"
  }
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### bdev_nvme_opal_revert {#rpc_bdev_nvme_opal_revert}

This is used to revert OPAL to its factory settings. Erase all user configuration and data.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
nvme_ctrlr_name         | Required | string      | name of nvme ctrlr
password                | Required | string      | admin password of OPAL

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "bdev_nvme_opal_revert",
  "id": 1,
  "params": {
    "nvme_ctrlr_name": "nvme0",
    "password": "*****"
  }
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### bdev_opal_create {#rpc_bdev_opal_create}

This is used to create an OPAL virtual bdev.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
nvme_ctrlr_name         | Required | string      | name of nvme ctrlr that supports OPAL
nsid                    | Required | number      | namespace ID
locking_range_id        | Required | number      | OPAL locking range ID
range_start             | Required | number      | locking range start LBA
range_length            | Required | number      | locking range length
password                | Required | string      | admin password of OPAL

#### Response

The response is the name of created OPAL virtual bdev.

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "bdev_opal_create",
  "id": 1,
  "params": {
    "nvme_ctrlr_name": "nvme0",
    "nsid": 1,
    "locking_range_id": 1,
    "range_start": 0,
    "range_length": 4096,
    "password": "*****"
  }
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": "nvme0n1r1"
}
~~~

### bdev_opal_get_info {#rpc_bdev_opal_get_info}

This is used to get information of a given OPAL bdev.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
bdev_name               | Required | string      | name of OPAL vbdev
password                | Required | string      | admin password

#### Response

The response is the locking info of OPAL virtual bdev.

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "bdev_opal_get_info",
  "id": 1,
  "params": {
    "bdev_name": "nvme0n1r1",
    "password": "*****"
  }
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
      "name": "nvme0n1r1",
      "range_start": 0,
      "range_length": 4096,
      "read_lock_enabled": true,
      "write_lock_enabled": true,
      "read_locked": false,
      "write_locked": false
    }
}
~~~

### bdev_opal_delete {#rpc_bdev_opal_delete}

This is used to delete OPAL vbdev.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
bdev_name               | Required | string      | name of OPAL vbdev
password                | Required | string      | admin password

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "bdev_opal_delete",
  "id": 1,
  "params": {
    "bdev_name": "nvme0n1r1",
    "password": "*****"
  }
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### bdev_opal_new_user {#rpc_bdev_opal_new_user}

This enables a new user to the specified opal bdev so that the user can lock/unlock the bdev.
Recalling this for the same opal bdev, only the newest user will have the privilege.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
bdev_name               | Required | string      | name of OPAL vbdev
admin_password          | Required | string      | admin password
user_id                 | Required | number      | user ID
user_password           | Required | string      | user password

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "bdev_opal_new_user",
  "id": 1,
  "params": {
    "bdev_name": "nvme0n1r1",
    "admin_password": "*****",
    "user_id": "1",
    "user_password": "********"
  }
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### bdev_opal_set_lock_state {#rpc_bdev_opal_set_lock_state}

This is used to lock/unlock specific opal bdev providing user ID and password.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
bdev_name               | Required | string      | name of OPAL vbdev
user_id                 | Required | number      | user ID
password                | Required | string      | user password
lock_state              | Required | string      | lock state

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "bdev_opal_set_lock_state",
  "id": 1,
  "params": {
    "bdev_name": "nvme0n1r1",
    "user_id": "1",
    "user_password": "********",
    "lock_state": "rwlock"
  }
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

## Notifications

### notify_get_types {#rpc_notify_get_types}

Return list of all supported notification types.

#### Parameters

None

#### Response

The response is an array of strings - supported RPC notification types.

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "notify_get_types",
  "id": 1
}
~~~

Example response:

~~~json
{
  "id": 1,
  "result": [
    "bdev_register",
    "bdev_unregister"
  ],
  "jsonrpc": "2.0"
}
~~~

### notify_get_notifications {#notify_get_notifications}

Request notifications. Returns array of notifications that happend since the specified id (or first that is available).

Notice: Notifications are kept in circular buffer with limited size. Older notifications might be inaccessible
due to being overwritten by new ones.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
id                      | Optional | number      | First Event ID to fetch (default: first available).
max                     | Optional | number      | Maximum number of event to return (default: no limit).

#### Response

Response is an array of event objects.

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
id                      | Optional | number      | Event ID.
type                    | Optional | number      | Type of the event.
ctx                     | Optional | string      | Event context.

#### Example

Example request:

~~~json
{
  "id": 1,
  "jsonrpc": "2.0",
  "method": "notify_get_notifications",
  "params": {
    "id": 1,
    "max": 10
  }
}

~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": [
    {
      "ctx": "Malloc0",
      "type": "bdev_register",
      "id": 1
    },
    {
      "ctx": "Malloc2",
      "type": "bdev_register",
      "id": 2
    }
  ]
}
~~~

## Linux Network Block Device (NBD) {#jsonrpc_components_nbd}

SPDK supports exporting bdevs through Linux nbd. These devices then appear as standard Linux kernel block devices
and can be accessed using standard utilities like fdisk.

In order to export a device over nbd, first make sure the Linux kernel nbd driver is loaded by running 'modprobe nbd'.

### nbd_start_disk {#rpc_nbd_start_disk}

Start to export one SPDK bdev as NBD disk

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
bdev_name               | Required | string      | Bdev name to export
nbd_device              | Optional | string      | NBD device name to assign

#### Response

Path of exported NBD disk

#### Example

Example request:

~~~json
{
 "params": {
    "nbd_device": "/dev/nbd1",
    "bdev_name": "Malloc1"
  },
  "jsonrpc": "2.0",
  "method": "nbd_start_disk",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": "/dev/nbd1"
}
~~~

### nbd_stop_disk {#rpc_nbd_stop_disk}

Stop one NBD disk which is based on SPDK bdev.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
nbd_device              | Required | string      | NBD device name to stop

#### Example

Example request:

~~~json
{
 "params": {
    "nbd_device": "/dev/nbd1",
  },
  "jsonrpc": "2.0",
  "method": "nbd_stop_disk",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": "true"
}
~~~

### nbd_get_disks {#rpc_nbd_get_disks}

Display all or specified NBD device list

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
nbd_device              | Optional | string      | NBD device name to display

#### Response

The response is an array of exported NBD devices and their corresponding SPDK bdev.

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "nbd_get_disks",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result":  [
    {
      "bdev_name": "Malloc0",
      "nbd_device": "/dev/nbd0"
    },
    {
      "bdev_name": "Malloc1",
      "nbd_device": "/dev/nbd1"
    }
  ]
}
~~~

## Blobfs {#jsonrpc_components_blobfs}

### blobfs_detect {#rpc_blobfs_detect}

Detect whether a blobfs exists on bdev.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
bdev_name               | Required | string      | Block device name to detect blobfs

#### Response

True if a blobfs exists on the bdev; False otherwise.

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "blobfs_detect",
  "params": {
    "bdev_name": "Malloc0"
  }
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": "true"
}
~~~

### blobfs_create {#rpc_blobfs_create}

Build blobfs on bdev.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
bdev_name               | Required | string      | Block device name to create blobfs
cluster_sz              | Optional | number      | Size of cluster in bytes. Must be multiple of 4KiB page size, default and minimal value is 1M.

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "blobfs_create",
  "params": {
    "bdev_name": "Malloc0",
    "cluster_sz": 1M
  }
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": "true"
}
~~~

### blobfs_mount {#rpc_blobfs_mount}

Mount a blobfs on bdev to one host path through FUSE

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
bdev_name               | Required | string      | Block device name where the blobfs is
mountpoint              | Required | string      | Mountpoint path in host to mount blobfs

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": ""blobfs_mount"",
  "params": {
    "bdev_name": "Malloc0",
    "mountpoint": "/mnt/"
  }
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": "true"
}
~~~

### blobfs_set_cache_size {#rpc_blobfs_set_cache_size}

Set cache pool size for blobfs filesystems.  This RPC is only permitted when the cache pool is not already initialized.

The cache pool is initialized when the first blobfs filesystem is initialized or loaded.  It is freed when the all
initialized or loaded filesystems are unloaded.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
size_in_mb              | Required | number      | Cache size in megabytes

#### Response

True if cache size is set successfully; False if failed to set.

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "blobfs_set_cache_size",
  "params": {
    "size_in_mb": 512,
  }
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

## Socket layer {#jsonrpc_components_sock}

### sock_impl_get_options {#rpc_sock_impl_get_options}

Get parameters for the socket layer implementation.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
impl_name               | Required | string      | Name of socket implementation, e.g. posix

#### Response

Response is an object with current socket layer options for requested implementation.

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "sock_impl_get_options",
  "id": 1,
  "params": {
    "impl_name": "posix"
  }
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "recv_buf_size": 2097152,
    "send_buf_size": 2097152,
    "enable_recv_pipe": true,
    "enable_quickack": true,
    "enable_placement_id": 0,
    "enable_zerocopy_send_server": true,
    "enable_zerocopy_send_client": false
  }
}
~~~

### sock_impl_set_options {#rpc_sock_impl_set_options}

Set parameters for the socket layer implementation.

#### Parameters

Name                        | Optional | Type        | Description
--------------------------- | -------- | ----------- | -----------
impl_name                   | Required | string      | Name of socket implementation, e.g. posix
recv_buf_size               | Optional | number      | Size of socket receive buffer in bytes
send_buf_size               | Optional | number      | Size of socket send buffer in bytes
enable_recv_pipe            | Optional | boolean     | Enable or disable receive pipe
enable_quick_ack            | Optional | boolean     | Enable or disable quick ACK
enable_placement_id         | Optional | number      | Enable or disable placement_id. 0:disable,1:incoming_napi,2:incoming_cpu
enable_zerocopy_send_server | Optional | boolean     | Enable or disable zero copy on send for server sockets
enable_zerocopy_send_client | Optional | boolean     | Enable or disable zero copy on send for client sockets

#### Response

True if socket layer options were set successfully.

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "sock_impl_set_options",
  "id": 1,
  "params": {
    "impl_name": "posix",
    "recv_buf_size": 2097152,
    "send_buf_size": 2097152,
    "enable_recv_pipe": false,
    "enable_quick_ack": false,
    "enable_placement_id": 0,
    "enable_zerocopy_send_server": true,
    "enable_zerocopy_send_client": false
  }
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### sock_set_default_impl {#rpc_sock_set_default_impl}

Set the default sock implementation.

#### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
impl_name               | Required | string      | Name of socket implementation, e.g. posix

#### Response

True if the default socket layer configuration was set successfully.

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "sock_set_default_impl",
  "id": 1,
  "params": {
    "impl_name": "posix"
  }
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

## Miscellaneous RPC commands

### bdev_nvme_send_cmd {#rpc_bdev_nvme_send_cmd}

Send NVMe command directly to NVMe controller or namespace. Parameters and responses encoded by base64 urlsafe need further processing.

Notice: bdev_nvme_send_cmd requires user to guarentee the correctness of NVMe command itself, and also optional parameters.
Illegal command contents or mismatching buffer size may result in unpredictable behavior.

#### Parameters

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

#### Response

Name                    | Type        | Description
----------------------- | ----------- | -----------
cpl                     | string      | NVMe completion queue entry, encoded by base64 urlsafe
data                    | string      | Data transferred from controller to host, encoded by base64 urlsafe
metadata                | string      | Metadata transferred from controller to host, encoded by base64 urlsafe

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "bdev_nvme_send_cmd",
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

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result":  {
    "cpl": "AAAAAAAAAAARAAAAWrmwABAA==",
    "data": "sIjg6AAAAACwiODoAAAAALCI4OgAAAAAAAYAAREAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
  }

}
~~~

### enable_vmd {#rpc_enable_vmd}

Enable VMD enumeration.

#### Parameters

This method has no parameters.

#### Response

Completion status of enumeration is returned as a boolean.

### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "enable_vmd",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### vmd_remove_device {#rpc_vmd_remove_device}

Remove a device behind a VMD.

### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
addr                    | Required | string      | Address of the device to remove.

### Example

~~~json
{
  "jsonrpc": "2.0",
  "method": "vmd_remove_device",
  "params": {
    "addr": "5d0505:01:00.0"
  }
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": true
}
~~~

### vmd_rescan {#rpc_vmd_rescan}

Force a rescan of the devices behind VMD.

### Parameters

This method has no parameters.

#### Response

The response is the number of new devices found.

### Example

~~~json
{
  "jsonrpc": "2.0",
  "method": "vmd_rescan",
  "id": 1
}
~~~

Example response:

~~~json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "count": 1
  }
}
~~~

### spdk_get_version {#rpc_spdk_get_version}

Get the version info of the running SPDK application.

#### Parameters

This method has no parameters.

#### Response

The response is the version number including major version number, minor version number, patch level number and suffix string.

#### Example

Example request:
~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "spdk_get_version"
}
~~

Example response:
~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "result":  {
    "version": "19.04-pre",
    "fields" : {
      "major": 19,
      "minor": 4,
      "patch": 0,
      "suffix": "-pre"
    }
  }
}
~~

### framework_get_pci_devices

List PCIe devices attached to an SPDK application and the contents of their config space.

#### Parameters

This method has no parameters.

#### Response

The response is an array of attached PCIe devices.

#### Example

Example request:
~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "framework_get_pci_devices"
}
~~

Example response:
Note that the config space buffer was trimmed.
~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    [
      {
        "address": "0000:00:04.0",
        "config_space": "8680455807051000...0000000000000000"
      },
      {
        "address": "0000:00:03.0",
        "config_space": "8680455807051000...0000000000000000"
      }
    ]
  }
}
~~
