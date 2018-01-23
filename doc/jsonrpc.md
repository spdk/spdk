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


# Block Device Abstraction Layer {#jsonrpc_components_bdev}

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
