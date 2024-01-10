# JSON-RPC 2.0 client in Go

[![Go Doc](https://img.shields.io/badge/godoc-reference-blue.svg)](http://godoc.org/github.com/spdk/spdk/go/rpc)
[![Go Report Card](https://goreportcard.com/badge/github.com/spdk/spdk/go/rpc)](https://goreportcard.com/report/github.com/spdk/spdk/go/rpc)

This directory contains JSON-RPC client written in Go. The main goal is to ease communication with
SPDK over Unix and TCP socket in Go. In addition, this repository provides integration of a client
with `rpc.py` - Go client replaces Python client.

## Client integration with rpc.py

### Build

Requirements:

* `go` (v1.21 or above)

There are two ways to build files required for client replacement:

1. Manual - go to [go/rpc](./) and invoke

```shell
make
```

2. During configuration part of SPDK add `--with-golang` flag.

### Usage

After successful installation you will be able to use Go client from within `rpc.py`

```shell
rpc.py --go-client RPC_COMMAND
```

If you want to use Go client for every RPC command of `rpc.py` without adding `--go-client` flag
you can define a new environment
variable

```bash
SPDK_JSONRPC_GO_CLIENT=1
```

## Examples

Examples how to integrate this client into your Go code can be
found [here](../../examples/go/hello_gorpc)

## API

### Client

Client is a main component of RPC client. It is responsible for sending and receiving JSON-RPC 2.0
calls.

#### Call

Sends RPC call with specified method and parameters.

Input:

- `method`: Name of the method to be invoked.
- `params`: Set of parameters to be used during invocation of the method.

Output:

- `response`: Response for rpc call. Contains version of JSON-RPC protocol, id, error or result.
- `error`: Contains error if something goes wrong during creation of a request or
 sending/receiving rpc call. Otherwise `nil`.

#### Close

Close method closes connection with underlying stream.

Output:

- `error`: Contains error when closing a stream fails. Otherwise `nil`.

### Request

Struct represents JSON-RPC 2.0 Request object. For more information please visit
[jsonrpc.org/specification#request_object](https://www.jsonrpc.org/specification#request_object)

### Response

Struct represents JSON-RPC 2.0 Response object. For more information please visit
[jsonrpc.org/specification#response_object](https://www.jsonrpc.org/specification#response_object)

### Error

Struct represents JSON-RPC 2.0 Error object. For more information please visit
[jsonrpc.org/specification#error_object](https://www.jsonrpc.org/specification#error_object)

### Other

#### CreateClientWithJsonCodec

This method creates a new JSON-RPC 2.0 client. Both Unix and TCP sockets are supported.

Input:

- `network`: Type of network. Both `unix` and `tcp` are supported.
- `address`: Address to given network.

Output:

- `client`: New [Client](#client) struct.
- `error`: Contains error if something goes wrong during creation of a client. Otherwise `nil`.
