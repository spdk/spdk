# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2025 Dell Inc, or its subsidiaries.  All rights reserved.

import os
from functools import partial, update_wrapper

from mcp.server.fastmcp import FastMCP  # type: ignore[import-not-found]

from spdk.rpc import bdev  # type: ignore[attr-defined]
from spdk.rpc.client import JSONRPCClient

# Create an MCP server
mcp = FastMCP("SPDK")

# Create an SPDK client
client = JSONRPCClient(addr=os.getenv("SPDK_RPC_ADDRESS", default="/var/tmp/spdk.sock"), port=8080)


@mcp.tool()
def spdk_get_version() -> dict:
    """Get the SPDK version"""
    return client.spdk_get_version()


# TODO: make this a loop over all rpc.*, not just bdev
bdev_functions = [y for x, y in bdev.__dict__.items() if x.startswith('bdev')]

# add all functions as tools to MCP server
for func in bdev_functions:
    newfunc = partial(func, client)
    update_wrapper(newfunc, func)
    mcp.add_tool(newfunc)

if __name__ == "__main__":
    mcp.run()
