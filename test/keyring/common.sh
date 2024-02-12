# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2024 Intel Corporation.  All rights reserved.

source "$rootdir/test/nvmf/common.sh"

bperfsock="/var/tmp/bperf.sock"

bperf_cmd() { "$rootdir/scripts/rpc.py" -s "$bperfsock" "$@"; }

get_key() { bperf_cmd keyring_get_keys | jq ".[] | select(.name == \"$1\")"; }

get_refcnt() { get_key "$1" | jq -r '.refcnt'; }

prep_key() {
	local name key digest path

	name="$1" key="$2" digest="$3"
	path="${4-$(mktemp)}"

	format_interchange_psk "$key" "$digest" > "$path"
	chmod 0600 "$path"

	echo "$path"
}
