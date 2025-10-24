#!/usr/bin/env python3
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) 2025 Dell Inc, or its subsidiaries.
#  All rights reserved.
#

import os
import re
import sys
import json
import argparse
from typing import Any, Dict, NoReturn
from pathlib import Path
from tabulate import tabulate
from jinja2 import Environment, FileSystemLoader

import rpc

# Get directory of this script
base_dir = Path(__file__).resolve().parent.parent


def lint_json_examples() -> None:
    with open(base_dir / "doc" / "jsonrpc.md.jinja2", "r") as file:
        data = file.read()
        examples = re.findall("~~~json(.+?)~~~", data, re.MULTILINE | re.DOTALL)
        for example in examples:
            try:
                json.loads(example)
            except json.decoder.JSONDecodeError:
                for i, x in enumerate(example.splitlines()):
                    print(i+1, x, file=sys.stderr)
                raise


def lint_py_cli(schema: Dict[str, Any]) -> None:
    types = {str: 'string', None: 'string', int: 'number', bool: 'boolean'}
    exceptions = {'load_config', 'load_subsystem_config', 'save_config', 'save_subsystem_config'}
    parser, subparsers = rpc.create_parser()
    schema_methods = set(method["name"] for method in schema['methods'])
    cli_methods = set(subparsers.choices.keys())
    missing_in_cli = schema_methods - cli_methods
    missing_in_schema = cli_methods - schema_methods - exceptions
    if missing_in_cli:
        raise ValueError(f"Methods defined in schema but missing in CLI: {sorted(missing_in_cli)}")
    if missing_in_schema:
        raise ValueError(f"Commands found in CLI but not defined in schema: {sorted(missing_in_schema)}")
    schema_objects = {obj["name"]: obj for obj in schema['objects']}
    for method in schema['methods']:
        subparser = subparsers.choices[method['name']]
        groups = subparser._mutually_exclusive_groups
        actions = {a.dest: a for a in subparser._actions}
        for parameter in method['params']:
            if parameter['name'] in ['num_blocks']:
                # TODO: handle this case later and fix issues raised by it
                continue
            params = schema_objects[parameter['class']]['fields'] if 'class' in parameter else [parameter]
            for param in params:
                action = actions.get(param['name'])
                if action is None:
                    raise ValueError(f"For method {method['name']}: parameter '{param['name']}': is defined in schema but not found in CLI")
                required = next((g.required for g in groups
                                if any(a.dest == action.dest for a in g._group_actions)),
                                action.required)
                if param['required'] != required:
                    raise ValueError(f"For method {method['name']}: parameter '{param['name']}': 'required' field is mismatched")
                newtype = 'boolean' if type(action) in [argparse._StoreTrueAction, argparse._StoreFalseAction] else types.get(action.type)
                if not newtype:
                    # TODO: handle this case later and fix issues raised by it
                    continue
                if param['type'] != newtype and action.metavar is None and param['type'] != "array":
                    raise ValueError(f"For method {method['name']}: parameter '{param['name']}': 'type' field is mismatched")


def generate_docs(schema: Dict[str, Any]) -> str:
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
    for obj in schema['objects']:
        fields = [
            dict(
                Name=el["name"],
                Optional="Required" if el["required"] else "Optional",
                Type=el["type"],
                Description=el["description"],
            )
            for el in obj["fields"]
        ]
        transformation[f"{obj['name']}_object"] = (
            tabulate(fields, headers="keys", tablefmt="presto").replace("-+-", " | ")
            if fields
            else "This method has no parameters."
        )
    return str(schema_template.render(transformation))


def generate_rpcs(schema: Dict[str, Any]) -> NoReturn:
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

    try:
        lint_json_examples()
    except ValueError as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)

    if not os.path.exists(args.schema):
        raise FileNotFoundError(f'Cannot access {args.schema}: No such file or directory')

    with open(args.schema, "r") as file:
        schema = json.load(file)

    try:
        lint_py_cli(schema)
    except ValueError as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)

    if args.doc:
        print(generate_docs(schema))
    if args.rpc:
        generate_rpcs(schema)
