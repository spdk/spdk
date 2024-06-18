#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2018 Intel Corporation
#  All rights reserved.
#
set -e

rootdir=$(readlink -f $(dirname "$0")/..)
scriptfile=$(basename "$0")

sma_goapi_dir=""                 # destination directory of generated go api
sma_goapi_ver=""                 # example: v1alpha1, parse from sma.proto go_package
sma_goapi_mod=""                 # example: github.com/spdk/sma-goapi, parse from sma.proto go_package
sma_proto_dir="${rootdir}/proto" # location of sma proto files

function usage() {
	echo "Usage: $0 -o SMA_GOAPI_DIR [-p PROTO_DIR]"
	echo
	echo "Generate SMA Go gRPC API from proto files."
	echo
	echo "SMA_GOAPI_DIR - target location for generating go.mod"
	echo "PROTO_DIR     - the location of sma.proto, the default is ${sma_proto_dir}"
	exit 0
}

function error() {
	echo "${scriptfile}:" "$@" >&2
	exit 1
}

function require() {
	local tool="$1"
	command -v "$tool" > /dev/null || error "missing $tool, make sure it is in PATH. (If installed with pkgdep.sh, source /etc/opt/spdk-pkgdep/paths/export.sh)"
}

function validate-inputs() {
	local go_package
	require go
	require protoc
	require protoc-gen-go

	[[ -n "${sma_goapi_dir}" ]] || error "missing -o SMA_GOAPI_DIR"

	[[ -f "${sma_proto_dir}/sma.proto" ]] || error "missing ${sma_proto_dir}/sma.proto"

	go_package=$(awk -F '"' '/option +go_package *=/{print $2}' < "${sma_proto_dir}/sma.proto")
	[[ -n "${go_package}" ]] || error "missing go_package in ${sma_proto_dir}/sma.proto"
	sma_goapi_ver="${go_package##*/}"
	sma_goapi_mod="${go_package%/*}"

	[[ "${sma_goapi_ver}" == v* ]] || error "invalid version in ${sma_proto_dir}/sma.proto, expecting vX[(alpha|beta)Y]"

	for src_proto in "${sma_proto_dir}"/*.proto; do
		grep -q "go_package.*=.*${sma_goapi_mod}/${sma_goapi_ver}" "${src_proto}" \
			|| error "expected go package ${sma_goapi_mod}/${sma_goapi_ver} in ${src_proto}"
	done
}

function generate() {
	local ver_dir="${sma_goapi_dir}/${sma_goapi_ver}"
	local dst_protos=()
	local dst_proto

	mkdir -p "${ver_dir}" || error "failed to create directory SMA_GOAPI_DIR/SMA_GOAPI_VER: ${ver_dir}"

	for src_proto in "${sma_proto_dir}"/*.proto; do
		dst_proto="${ver_dir}/$(basename "$src_proto")"
		cp "$src_proto" "$dst_proto" || error "failed to copy '${src_proto}' to '${dst_proto}'"
		dst_protos+=("${dst_proto}")
	done

	for dst_proto in "${dst_protos[@]}"; do
		protoc --go_out="${ver_dir}" \
			--go_opt=module="${sma_goapi_mod}/${sma_goapi_ver}" \
			--go-grpc_out="${ver_dir}" \
			--go-grpc_opt=module="${sma_goapi_mod}/${sma_goapi_ver}" \
			--proto_path="${ver_dir}" \
			"${dst_proto}"
	done

	(
		cd "${sma_goapi_dir}" || error "cannot change directory to ${sma_goapi_dir}"
		rm -f go.mod go.sum
		go mod init "${sma_goapi_mod}" || error "go mod init ${sma_goapi_mod} failed in ${sma_goapi_dir}"
		go mod tidy || error "go mod tidy failed in ${sma_goapi_dir}"
	)
}

while getopts 'ho:p:-:' optchar; do
	case "$optchar" in
		-)
			case "$OPTARG" in
				help) usage ;;
				*) error "invalid argument '$OPTARG'" ;;
			esac
			;;
		h) usage ;;
		o) sma_goapi_dir=$OPTARG ;;
		p) sma_proto_dir=$OPTARG ;;
		*) error "invalid argument '$OPTARG'" ;;
	esac
done

validate-inputs
generate
