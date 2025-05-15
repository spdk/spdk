# SPDK Python Bindings and Scripts

[![PyPI Latest Release](https://img.shields.io/pypi/v/spdk.svg)](https://pypi.org/project/spdk/)
[![PyPI Downloads](https://img.shields.io/pypi/dm/spdk.svg?label=PyPI%20downloads)](https://pypi.org/project/spdk/)

## Overview

The Storage Performance Development Kit ([SPDK](http://www.spdk.io)) provides a set of tools
and libraries for writing high performance, scalable, user-mode storage
applications. This directory contains Python bindings and scripts that facilitate interaction with SPDK components.

## Installation

Examples below are using [UV](https://docs.astral.sh/uv/getting-started/installation/).

An extremely fast Python package and project manager, written in Rust.

```shell
$ uv venv
$ source .venv/bin/activate
```

Then, you can install spdk from PyPI with:

```shell
uv pip install spdk
```

or install from source with:

```shell
git clone https://github.com/spdk/spdk.git
cd spdk
uv pip install python/
```

## Getting Started

From a shell:

```bash
spdk-rpc rpc_get_methods
```

From a Python interpreter:

```python
>>> from spdk.rpc.client import JSONRPCClient

>>> client = JSONRPCClient(server_addr, port)
>>> client.call("rpc_get_methods")
```

For more information, see <https://spdk.io/doc/jsonrpc.html>
