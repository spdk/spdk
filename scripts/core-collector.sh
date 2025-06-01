#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2020 Intel Corporation
#  All rights reserved.
#

shopt -s nullglob

core_meta() {
	jq . <<- CORE
		{
		  "$exe_comm": {
		    "ts": "$core_time",
		    "size": "$core_size bytes",
		    "PID": $core_pid,
		    "TID": $core_thread,
		    "signal": "$core_sig ($core_sig_name)",
		    "path": "$exe_path"
		  }
		}
	CORE
}

bt() { hash gdb && gdb -batch -ex "thread apply all bt full" "$1" "$2" 2>&1; }

parse_core() {
	local core=$1 _core
	local cores_dir=$2

	local core_pid core_thread
	local core_save
	local core_sig core_sig_name
	local core_size
	local core_time
	local exe_comm exe_pat

	local prefix=(
		core_sig
		core_pid
		core_thread
		core_time
	)

	# $output_dir/coredumps/%s-%p-%i-%t-%E.core
	#  |
	#  v
	#  11-47378-47378-1748807733-!opt!spdk!build!bin!spdk_tgt.core
	_core=${core##*/} _core=${_core%.core}
	# !opt!spdk!build!bin!spdk_tgt
	exe_path=${_core#*-*-*-*-}
	# Split 11-47378-47378-1748807733 into respective variables
	IFS="-" read -r "${prefix[@]}" <<< "${_core%"-$exe_path"}"
	# /opt/spdk/build/bin/spdk_tgt
	exe_path=${exe_path//\!/\/}
	# spdk_tgt
	exe_comm=${exe_path##*/}
	# 11 -> SEGV
	core_sig_name=$(kill -l "$core_sig")
	# seconds since Epoch to date
	core_time=$(date -d"@$core_time")
	# size in bytes
	core_size=$(wc -c < "$core")
	# $output_dir/coredumps/spdk_tgt-47378-47378
	core_save=$cores_dir/$exe_comm-$core_pid-$core_thread

	# Compress it
	gzip -c "$core" > "$core_save.gz"
	# Save the binary
	cp "$exe_path" "$core_save.bin"
	# Save the backtrace
	bt "$exe_path" "$core" > "$core_save.bt.txt"
	# Save the metadata of the core
	core_meta > "$core_save.json"
	# Nuke the original core
	rm "$core"
}

cores_dir=$1

cores=("$cores_dir/"*.core)
((${#cores[@]} > 0)) || exit 0

for core in "${cores[@]}"; do
	parse_core "$core" "$cores_dir"
done
