#!/usr/bin/env bash
testdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$testdir/../../../")
source "$rootdir/test/common/autotest_common.sh"

nvmes=("$@") all_nvmes=${#nvmes[@]}

# We need 2 ctrls at minimum
((all_nvmes >= 2))

# Let each ctrl to have its CMB copied to the other device.
while ((--all_nvmes >= 0)); do
	read_nvme=${nvmes[all_nvmes]}
	for nvme_idx in "${!nvmes[@]}"; do
		[[ ${nvmes[nvme_idx]} == "$read_nvme" ]] && continue
		"$rootdir/build/examples/cmb_copy" \
			-r "$read_nvme-1-0-1" \
			-w "${nvmes[nvme_idx]}-1-0-1" \
			-c "$read_nvme"
	done
done
