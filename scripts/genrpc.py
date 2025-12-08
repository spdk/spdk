#!/usr/bin/env python3
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) 2025 Dell Inc, or its subsidiaries.
#  All rights reserved.
#

import argparse
import json
import os
import re
import sys
from pathlib import Path
from typing import Any, Dict, NoReturn

import rpc
from jinja2 import Environment, FileSystemLoader, Template
from tabulate import tabulate

# Get directory of this script
base_dir = Path(__file__).resolve().parent.parent


def lint_json_examples() -> None:
    with open(base_dir / "doc" / "jsonrpc.md.jinja2", "r") as file:
        data = file.read()
        examples = re.findall("~~~json(.+?)~~~", data, re.MULTILINE | re.DOTALL)
        for example in examples:
            try:
                t = Template(example)
                rendered = t.render(all_methods=["rpc_get_methods"])
                json.loads(rendered)
            except json.decoder.JSONDecodeError:
                for i, x in enumerate(example.splitlines()):
                    print(i+1, x, file=sys.stderr)
                raise


def lint_c_code(schema: Dict[str, Any]) -> None:
    schema_methods = set(method["name"] for method in schema['methods'])
    schema_objects = {obj["name"]: obj for obj in schema['objects']}
    schema_decoders = {method["name"]:method["decoder"] for method in schema['methods'] if "decoder" in method}
    schema_aliases = {method["name"]:method["alias"] for method in schema['methods'] if "alias" in method}
    exception_methods = {"nvmf_create_target", "nvmf_delete_target", "nvmf_get_targets"}
    # TODO: those are embeeded objects decoders and will be resolved soon
    exceptions_decoders = {f"rpc_{name}_decoders" for name in schema_objects}
    c_code_methods = dict()
    c_code_aliases = dict()
    for folder in ("module", "lib"):
        for path in (base_dir / folder).rglob("*rpc.c"):
            data = path.read_text()
            methods = re.findall(r'SPDK_RPC_REGISTER\("([A-Za-z0-9_]+)"\s*,\s*([A-Za-z0-9_]+)\s*,', data, re.MULTILINE)
            for name, func in methods:
                if func != f"rpc_{name}":
                    raise ValueError(f"In file {path}: RPC name '{name}' does not match function name '{func}'")
                if name not in schema_methods | exception_methods:
                    raise ValueError(f"In file {path}: RPC name '{name}' does not appear in schema. Update schema or exception list")
            aliases = re.findall(r'SPDK_RPC_REGISTER_ALIAS_DEPRECATED\(\s*([A-Za-z0-9_]+)\s*,\s*([A-Za-z0-9_]+)\s*\)', data)
            c_code_aliases.update(aliases)
            decoders = re.findall(r"static\s+const\s+struct\s+spdk_json_object_decoder\s+(.+?)\[\]\s+=\s+{(.+?)};",
                                  data, re.MULTILINE | re.DOTALL)
            struct_names = {schema_decoders.get(name, f"rpc_{name}_decoders") for name, _ in methods}
            decoder_names = {name for name, _ in decoders}
            invalid = decoder_names - exceptions_decoders - struct_names
            if invalid:
                raise ValueError(f"In file {path}: RPC names {invalid} do not match available decoders: {struct_names}."
                                "Update decoder names or exception list.")
            for name, fields in decoders:
                c_code_methods[name] = re.findall(r'\{\s*"(\w+)",\s*(offsetof\(.+?\)|0),\s*(\w+)', fields, re.MULTILINE | re.DOTALL)
                if not c_code_methods[name]:
                    raise ValueError(f"In file {path}: could not parse fields for decoder '{name}' fields: '{fields}'. Fix decoder code.")
    if c_code_aliases != schema_aliases:
        raise ValueError(f"Aliases {c_code_aliases} do not match schema aliases {schema_aliases}. Update schema aliases or code c.")
    for method in schema['methods']:
        decoder_name = schema_decoders.get(method['name'], f"rpc_{method['name']}_decoders")
        schema_params = set(parameter["name"] for parameter in method['params'])
        # if there are no params, there will be no decoder
        if not schema_params and decoder_name not in c_code_methods:
            continue
        if not c_code_methods.get(decoder_name, {}):
            raise ValueError(f"Decoder of '{method['name']}' named '{decoder_name}' was not found. Update decoder names or exception list.")
        cli_params = set(n for n, o, t in c_code_methods[decoder_name])
        missing_in_cli = schema_params - cli_params
        missing_in_schema = cli_params - schema_params
        if missing_in_cli:
            # TODO: handle this case later and fix issues raised by it
            cli_exceptions = {'framework_set_scheduler', 'nvmf_create_transport', 'vhost_create_blk_controller', 'nvmf_subsystem_add_host'}
            if method['name'] not in cli_exceptions:
                raise ValueError(f"Params of '{method['name']}' defined in schema but missing in CLI: {sorted(missing_in_cli)}")
        if missing_in_schema:
            raise ValueError(f"Params of '{method['name']}' defined in CLI but missing in schema: {sorted(missing_in_schema)}")


def lint_py_cli(schema: Dict[str, Any]) -> None:
    types = {str: 'string', None: 'string', int: 'number', bool: 'boolean'}
    exceptions = {'load_config', 'load_subsystem_config', 'save_config', 'save_subsystem_config'}
    parser, subparsers = rpc.create_parser()
    schema_methods = set(method["name"] for method in schema['methods'])
    class_methods = set(dir(rpc.JSONRPCClient))
    conflicts = schema_methods & class_methods
    if conflicts:
        raise Exception(f"JSONRPCClient methods already exist, so can't name RPC same: {conflicts}")
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
        # Those are not part of the schema, just part of the python cli
        p_schema_exceptions = {'help', 'total_size', 'format_lspci'}
        p_cli_exceptions = {'num_blocks'}
        p_params = [schema_objects[parameter['class']]['fields'] if 'class' in parameter else [parameter] for parameter in method['params']]
        p_params_set = set([p['name'] for sub in p_params for p in sub])
        p_missing_in_cli = p_params_set - set(actions) - p_cli_exceptions
        p_missing_in_schema = set(actions) - p_params_set - p_schema_exceptions
        if p_missing_in_cli:
            raise ValueError(f"For method {method['name']}: Params defined in schema but missing in CLI: {sorted(p_missing_in_cli)}")
        if p_missing_in_schema:
            raise ValueError(f"For method {method['name']}: Params found in CLI but not defined in schema: {sorted(p_missing_in_schema)}")
        for parameters_list in p_params:
            for param in parameters_list:
                if param['name'] in p_cli_exceptions:
                    # TODO: handle this case later and fix issues raised by it
                    continue
                action = actions.get(param['name'])
                if action is None:
                    raise ValueError(f"For method {method['name']}: parameter '{param['name']}': is defined in schema but not found in CLI")
                required = next((g.required for g in groups
                                if any(a.dest == action.dest for a in g._group_actions)),
                                action.required)
                if param.get('required', False) != required:
                    raise ValueError(f"For method {method['name']}: parameter '{param['name']}': 'required' field is mismatched")
                if type(action) in [argparse._StoreTrueAction, argparse._StoreFalseAction, argparse.BooleanOptionalAction]:
                    newtype = 'boolean'
                else:
                    newtype = types.get(action.type)
                if not newtype:
                    # TODO: handle this case later and fix issues raised by it
                    continue
                if param['type'] != newtype and action.metavar is None and param['type'] != "array":
                    raise ValueError(f"For method {method['name']}: parameter '{param['name']}': 'type' field is mismatched")


def generate_docs(schema: Dict[str, Any]) -> str:
    env = Environment(loader=FileSystemLoader(base_dir / "doc"),
                      keep_trailing_newline=True,
                      comment_start_string='<!--',
                      comment_end_string='-->',
                      )
    schema_template = env.get_template('jsonrpc.md.jinja2')
    transformation = dict()
    transformation["all_methods"] = [method['name'] for method in schema['methods']]
    for method in schema['methods']:
        params = [
            dict(
                Name=el["name"],
                Optional="Required" if el.get("required", False) else "Optional",
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
                Optional="Required" if el.get("required", False) else "Optional",
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
        description="RPC functions and documentation generator",
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
        lint_c_code(schema)
    except ValueError as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)

    if args.doc:
        print(generate_docs(schema))
    if args.rpc:
        generate_rpcs(schema)
