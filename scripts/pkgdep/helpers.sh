#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2024 Intel Corporation
#  All rights reserved.
#

pkgdep_toolpath() {
	# Usage: pkgdep_toolpath TOOL DIR
	#
	# Regenerates /etc/opt/spdk-pkgdep/paths to ensure that
	# TOOL in DIR will be in PATH before other versions
	# of the TOOL installed in the system.
	local toolname="$1"
	local toolpath="$2"
	local toolpath_dir="/etc/opt/spdk-pkgdep/paths"
	local toolpath_file="${toolpath_dir}/${toolname}.path"
	local export_file="${toolpath_dir}/export.sh"
	mkdir -p "$(dirname "${toolpath_file}")"
	echo "${toolpath}" > "${toolpath_file}" || {
		echo "cannot write toolpath ${toolpath} to ${toolpath_file}"
		return 1
	}
	echo "# generated, source this file in shell" > "${export_file}"
	for pathfile in "${toolpath_dir}"/*.path; do
		echo "PATH=$(< ${pathfile}):\$PATH" >> "${export_file}"
	done
	echo "export PATH" >> "${export_file}"
	echo "echo \$PATH" >> "${export_file}"
	chmod a+x "${export_file}"
}

pkgdep_setup_python_venv() {
	# Usage: pkgdep_setup_python_venv ROOTDIR
	#
	# Sets up Python virtual environment and installs dependencies
	# per PEP668 work inside virtual env
	local rootdir="$1"
	local virtdir=${PIP_VIRTDIR:-/var/spdk/dependencies/pip}

	python3 -m venv --system-site-packages "$virtdir"
	source "$virtdir/bin/activate"
	python -m pip install -U "pip<26" setuptools wheel pip-tools
	pip-compile --extra dev --strip-extras -o "$rootdir/scripts/pkgdep/requirements.txt" "${rootdir}/python/pyproject.toml"
	pip3 install -r "$rootdir/scripts/pkgdep/requirements.txt"

	# Fixes issue: #3721
	pkgdep_toolpath meson "${virtdir}/bin"
}
