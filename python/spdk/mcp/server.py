# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2025 Dell Inc, or its subsidiaries.  All rights reserved.

from spdk import rpc
from spdk.rpc.client import JSONRPCClient
from mcp.server.fastmcp import FastMCP

# Create an MCP server
mcp = FastMCP("SPDK")

# TODO: add tcp support and make it input argument
client = JSONRPCClient("/var/tmp/spdk.sock")


@mcp.tool()
def spdk_get_version() -> dict:
    """Get the SPDK version"""
    return rpc.spdk_get_version(client)


@mcp.resource("bdevs://all")
def bdev_get_bdevs() -> list:
    """Get a list of block devices"""
    return rpc.bdev.bdev_get_bdevs(client)


@mcp.tool()
def bdev_malloc_create(name: str, num_blocks: int = 1024, block_size: int = 512) -> dict:
    """Create a malloc block device"""
    return rpc.bdev.bdev_malloc_create(client, num_blocks, block_size, name=name)
