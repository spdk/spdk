# JSON-RPC Methods {#jsonrpc}

# Overview {#jsonrpc_overview}

SPDK implements a [JSON-RPC 2.0](http://www.jsonrpc.org/specification) server
to allow external management tools to dynamically configure SPDK components.

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
      "core": 0,
      "nqn": "nqn.2014-08.org.nvmexpress.discovery",
      "subtype": "Discovery"
      "listen_addresses": [],
      "hosts": [],
      "allow_any_host": true
    },
    {
      "core": 5,
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
core                    | Optional | number      | Core to run the subsystem's poller on. Default: Automatically assign a core.
nqn                     | Required | string      | Subsystem NQN
listen_addresses        | Required | array       | Array of @ref rpc_construct_nvmf_subsystem_listen_address objects
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

### Example

Example request:

~~~
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "construct_nvmf_subsystem",
  "params": {
    "core": 5,
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
