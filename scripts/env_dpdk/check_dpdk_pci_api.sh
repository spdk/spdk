#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2022 Intel Corporation
#  All rights reserved.
#

scriptdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$scriptdir/../..")
source "$rootdir/scripts/common.sh"

set -e
shopt -s extglob

mode=${1:-check} # check or fix

# By default verify headers matching the DPDK submodule
dpdk_dir=${2:-"$rootdir/dpdk"}
dpdk_ver=$(< "$dpdk_dir/VERSION")

env_path="$rootdir/lib/env_dpdk"
tracked_versions=("$env_path/"+([0-9]).+([0-9])/*.h)
tracked_versions=("${tracked_versions[@]#"$env_path/"}")
tracked_versions=("${tracked_versions[@]%/*}")

# The DPDK PCI API tracking started with DPDK 22.11, all prior versions will use DPDK 22.07 headers
target_ver="22.07"
while read -r ver; do
	ge "$dpdk_ver" "$ver" && target_ver=$ver && break
done < <(printf "%s\n" "${tracked_versions[@]}" | sort -Vru)

echo "Checking DPDK PCI API from $dpdk_ver against $target_ver ..."

target_headers=("$env_path/$target_ver/"*.h)
target_headers=("${target_headers[@]##*/}")

# The includes should point to headers in SPDK tree rather than system ones.
use_local_includes="-e "
for header in "${target_headers[@]}"; do
	use_local_includes+="s/#include <$header>/#include \"$header\"/g;"
done

for header in "${target_headers[@]}"; do
	dpdk_file="$dpdk_dir/$(git -C "$dpdk_dir" ls-files "*/$header")"

	# Patch DPDK header with any workarounds necessary
	patch_file="$scriptdir/$target_ver/$header.patch"
	if [[ -s $patch_file ]]; then
		dpdk_header=$(patch -s "$dpdk_file" "$patch_file" -o - | sed "$use_local_includes")
	else
		dpdk_header=$(sed "$use_local_includes" "$dpdk_file")
	fi

	spdk_file="$env_path/$target_ver/$header"
	if ! single_diff=$(diff -u "$spdk_file" <(echo "$dpdk_header")); then
		header_diff+="$single_diff\n"
	fi
done

if [[ -z "$header_diff" ]]; then
	echo "No differences in headers found."
	exit 0
fi

if [[ "$mode" == "check" ]]; then
	cat <<- CHECK
		$(echo -e "$header_diff")

		Differences in DPDK and internal SPDK headers found.
		For changes that do not affect the API, please use 'fix' as \$1 to this script.
		If API was changed, please create "$env_path/$dpdk_ver/" with appropriate headers.

	CHECK
elif [[ "$mode" == "fix" ]]; then
	echo -e "$header_diff" | patch -d "$env_path/$target_ver/"
	echo "Fixed differences between DPDK and internal SPDK headers."
else
	echo "Incorrect \$1 passed, please use 'check' or 'fix'."
	exit 1
fi
