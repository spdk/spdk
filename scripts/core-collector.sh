#!/usr/bin/env bash
# We don't want to tell kernel to include %e or %E since these
# can include whitespaces or other funny characters, and working
# with those on the cmdline would be a nightmare. Use procfs for
# the remaining pieces we want to gather:
# |$rootdir/scripts/core-collector.sh %P %s %t %c $output_dir

core_meta() {
	jq . <<- CORE
		{
		  "$exe_comm": {
		    "ts": "$core_time",
		    "size": "$core_size bytes",
		    "PID": $core_pid,
		    "signal": "$core_sig ($core_sig_name)",
		    "path": "$exe_path",
		    "statm": "$statm"
		  }
		}
	CORE
}

bt() { hash gdb && gdb -batch -ex "thread apply all bt full" "$1" "$2" 2>&1; }

stderr() {
	exec 2> "$core.stderr.txt"
	set -x
}

args+=(core_pid)
args+=(core_sig)
args+=(core_ts)
args+=(rlimit)

read -r "${args[@]}" <<< "$*"

exe_path=$(readlink -f "/proc/$core_pid/exe")
exe_comm=$(< "/proc/$core_pid/comm")
statm=$(< "/proc/$core_pid/statm")
core_time=$(date -d@"$core_ts")
core_sig_name=$(kill -l "$core_sig")

core=$(< "${0%/*}/../.coredump_path")/${exe_path##*/}_$core_pid.core
stderr

# RLIMIT_CORE is not enforced when core is piped to us. To make
# sure we won't attempt to overload underlying storage, copy
# only the reasonable amount of bytes (systemd defaults to 2G
# so let's follow that). But first, check limits of terminating
# process to see if we need to make any adjustments.
max_core=$((1024 * 1024 * 1024 * 2))

if ((rlimit == 0xffffffffffffffff || rlimit > max_core)); then
	rlimit=$max_core
fi

# Clear path for lz
rm -f "$core"{,.{bin,bt,gz,json}}

# Slurp the core
head -c "$rlimit" <&0 > "$core"
core_size=$(wc -c < "$core")

# Compress it
gzip -c "$core" > "$core.gz"

# Save the binary
cp "$exe_path" "$core.bin"

# Save the backtrace
bt "$exe_path" "$core" > "$core.bt.txt"

# Save the metadata of the core
core_meta > "$core.json"

# Nuke the original core
rm "$core"
