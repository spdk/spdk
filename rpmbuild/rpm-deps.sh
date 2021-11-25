#!/usr/bin/env bash
# This script simply iterates over all libs SPDK binaries link
# to and returns a list of .rpm packages SPDK may depend on. At
# the end, the list strictly relates to how the SPDK build was
# ./configure'd.

shopt -s nullglob

rpmdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$rpmdir/../")
rc=0

bins=("$rootdir/"build/{bin,examples}/*)
(($#)) && bins=("$@")

((${#bins[@]} > 0)) || exit 0

source /etc/os-release

id_ok=no

for id in $ID $ID_LIKE; do
	[[ "$id" =~ ^(fedora|centos|rhel) ]] && id_ok=yes
done

if [[ "$id_ok" != "yes" ]]; then
	exit 0
fi

declare -A deps=()
for bin in "${bins[@]}"; do
	if ! type -P "$bin"; then
		printf '%s is missing\n' "$bin" >&2
		rc=1
		continue
	fi
	while read -r name _ lib _; do
		[[ -n $lib ]] || continue
		[[ -z ${deps["$lib"]} ]] || continue
		if [[ ! -e $lib ]]; then
			lib=$name pkg="missing"
			rc=1
		elif ! pkg=$(rpm -qf "$lib"); then
			pkg=${lib##*/}
		fi
		deps["$lib"]=$pkg
	done < <(LD_TRACE_LOADED_OBJECTS=1 "$bin")
done

if [[ -n $LIST_LIBS ]]; then
	for lib in "${!deps[@]}"; do
		echo "$lib:${deps["$lib"]}"
	done
else
	printf '%s\n' "${deps[@]}"
fi | sort -u

((rc == 0))
