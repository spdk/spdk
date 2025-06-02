#!/usr/bin/env python3
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) 2025 Dell Inc, or its subsidiaries.
#  All rights reserved.
#

import os
import json
import argparse
from pathlib import Path
from tabulate import tabulate
from jinja2 import Environment, FileSystemLoader

# Get directory of this script
base_dir = Path(__file__).resolve().parent.parent


def generate_docs(schema):
    env = Environment(loader=FileSystemLoader(base_dir / "doc"),
                      keep_trailing_newline=True,
                      comment_start_string='<!--',
                      comment_end_string='-->'
                      )
    schema_template = env.get_template('jsonrpc.md.jinja2')
    transformation = dict()
    for method in schema['methods']:
        params = [
            dict(
                Name=el["name"],
                Optional="Required" if el["required"] else "Optional",
                Type=el["type"],
                Description=el["description"],
            )
            for el in method["params"]
        ]
        transformation[f"{method['name']}_params"] = (
            tabulate(params, headers="keys", tablefmt="presto").replace("-+-", " | ")
            if params
            else "This method has no parameters."
        )
    result = schema_template.render(transformation)
    print(result)


def generate_rpcs(schema):
    raise NotImplementedError("Auto generating python/c code for rpc is not yet implemented")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="RPC functions and documentation generator"
    )
    parser.add_argument(
        "-s",
        "--schema",
        dest="schema",
        help="path to rpc json schema",
        required=True,
    )
    parser.add_argument(
        "-d",
        "--doc",
        dest="doc",
        help="run rpc doc generation",
        required=False,
        action="store_true",
    )
    parser.add_argument(
        "-r",
        "--rpcs",
        dest="rpc",
        help="run rpc code generation",
        required=False,
        action="store_true",
    )

    args = parser.parse_args()

    if not os.path.exists(args.schema):
        raise FileNotFoundError(f'Cannot access {args.schema}: No such file or directory')

    with open(args.schema, "r") as file:
        schema = json.load(file)

    if not (args.doc or args.rpc):
        parser.error("No action requested, add -d for doc or -r for rpcs generation")

    if args.doc:
        generate_docs(schema)
    if args.rpc:
        generate_rpcs(schema)
