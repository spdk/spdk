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
>>> client.rpc_get_methods()
```

For more information, see <https://spdk.io/doc/jsonrpc.html>

## Model Context Protocol (MCP)

[![Install in VS Code](https://img.shields.io/badge/VS_Code-Install_SPDK-0098FF?style=flat-square&logo=visualstudiocode&logoColor=white)](https://insiders.vscode.dev/redirect/mcp/install?name=spdk&config=%7B%22name%22%3A%22spdk%22%2C%22command%22%3A%22uvx%22%2C%22args%22%3A%5B%22spdk-mcp%22%5D%2C%22env%22%3A%7B%22SPDK_RPC_ADDRESS%22%3A%22%2Fvar%2Ftmp%2Fspdk.sock%22%7D%7D)

The [Model Context Protocol (MCP)](https://modelcontextprotocol.io/) lets you build servers that expose
data and functionality to LLM applications in a secure, standardized way.
Think of it like a web API, but specifically designed for LLM interactions.

MCP servers can:

- Expose data through Resources (think of these sort of like GET endpoints; they are used to load information into the LLM's context)
- Provide functionality through Tools (sort of like POST endpoints; they are used to execute code or otherwise produce a side effect)
- Define interaction patterns through Prompts (reusable templates for LLM interactions)
- And more!

The SPDK MCP Server is a Model Context Protocol (MCP) server that provides seamless integration with SPDK APIs,
enabling advanced automation and interaction capabilities for developers and tools.

```bash
$ uv venv
$ source .venv/bin/activate
$ uv pip install spdk[mcp]
or locally
$ uv pip install python/[mcp]
```

You can install this server in [Claude Desktop](https://claude.ai/download) and interact with it right away by running:

```bash
mcp install server.py
```

Alternatively, you can test it with the MCP Inspector:

```bash
mcp dev server.py
```
