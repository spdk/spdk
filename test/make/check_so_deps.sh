#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2019 Intel Corporation
#  All rights reserved.
#
shopt -s extglob
git_repo_abi="https://github.com/spdk/spdk-abi.git"

function get_spdk_abi() {
	local dest=$1
	mkdir -p $dest
	if [[ -d $SPDK_ABI_DIR ]]; then
		echo "spdk-abi found at $SPDK_ABI_DIR"
		cp -r "$SPDK_ABI_DIR"/* "$dest/"
	else
		# In case that someone run test manually and did not set existing
		# spdk-abi directory via SPDK_ABI_DIR
		echo "spdk-abi has not been found at $SPDK_ABI_DIR, cloning"
		git clone $git_repo_abi "$dest"
	fi
}

function get_release_branch() {
	tag=$(git describe --tags --abbrev=0 --exclude=LTS --exclude=*-pre)
	branch="${tag:0:6}.x"
	echo "$branch"
}

if [ "$(uname -s)" = "FreeBSD" ]; then
	echo "Not testing for shared object dependencies on FreeBSD."
	exit 0
fi

rootdir=$(readlink -f $(dirname $0)/../..)

if [[ ! -f $1 ]]; then
	echo "ERROR: SPDK test configuration not specified"
	exit 1
fi

source $1
source "$rootdir/test/common/autotest_common.sh"

libdir="$rootdir/build/lib"
libdeps_file="$rootdir/mk/spdk.lib_deps.mk"
suppression_file="$HOME/abigail_suppressions.ini"

function check_header_filenames() {
	local dups_found=0

	xtrace_disable

	include_headers=$(git ls-files -- $rootdir/include/spdk $rootdir/include/spdk_internal | xargs -n 1 basename)
	dups=
	for file in $include_headers; do
		if [[ $(git ls-files "$rootdir/lib/**/$file" "$rootdir/module/**/$file" --error-unmatch 2> /dev/null) ]]; then
			dups+=" $file"
			dups_found=1
		fi
	done

	xtrace_restore

	if ((dups_found == 1)); then
		echo "Private header file(s) found with same name as public header file."
		echo "This is not allowed since it can confuse abidiff when determining if"
		echo "data structure changes impact ABI."
		echo $dups
		return 1
	fi
}

function confirm_abi_deps() {
	local processed_so=0
	local abidiff_output
	local release
	local source_abi_dir="$rootdir/test/make/abi"

	release=$(get_release_branch)

	get_spdk_abi "$source_abi_dir"
	echo "* Running ${FUNCNAME[0]} against the latest (${release%.*}) release" >&2

	if ! hash abidiff; then
		echo "Unable to check ABI compatibility. Please install abidiff."
		return 1
	fi

	cat << EOF > ${suppression_file}
[suppress_type]
	name = spdk_nvme_power_state
[suppress_type]
	name = spdk_nvme_ctrlr_data
[suppress_type]
	name = spdk_nvme_cdata_oacs
[suppress_type]
	name = spdk_nvme_cdata_nvmf_specific
[suppress_type]
	name = spdk_nvme_cmd
[suppress_type]
	name = spdk_bs_opts
[suppress_type]
	name = spdk_app_opts
EOF

	for object in "$libdir"/libspdk_*.so; do
		abidiff_output=0

		so_file=$(basename $object)
		if [ ! -f "$source_abi_dir/$release/$so_file" ]; then
			echo "No corresponding object for $so_file in canonical directory. Skipping."
			continue
		fi

		cmd_args=('abidiff'
			$source_abi_dir/$release/$so_file "$libdir/$so_file"
			'--headers-dir1' $source_abi_dir/$release/include
			'--headers-dir2' $rootdir/include
			'--leaf-changes-only' '--suppressions' $suppression_file)

		if ! output=$("${cmd_args[@]}" --stat); then
			# remove any filtered out variables.
			output=$(sed "s/ [()][^)]*[)]//g" <<< "$output")

			IFS="." read -r _ _ new_so_maj new_so_min < <(readlink "$libdir/$so_file")
			IFS="." read -r _ _ old_so_maj old_so_min < <(readlink "$source_abi_dir/$release/$so_file")

			found_abi_change=false
			so_name_changed=no

			if [[ $output == *"ELF SONAME changed"* ]]; then
				so_name_changed=yes
			fi

			changed_leaf_types=0
			if [[ $output =~ "leaf types summary: "([0-9]+) ]]; then
				changed_leaf_types=${BASH_REMATCH[1]}
			fi

			removed_functions=0 changed_functions=0 added_functions=0
			if [[ $output =~ "functions summary: "([0-9]+)" Removed, "([0-9]+)" Changed, "([0-9]+)" Added" ]]; then
				removed_functions=${BASH_REMATCH[1]} changed_functions=${BASH_REMATCH[2]} added_functions=${BASH_REMATCH[3]}
			fi

			removed_vars=0 changed_vars=0 added_vars=0
			if [[ $output =~ "variables summary: "([0-9]+)" Removed, "([0-9]+)" Changed, "([0-9]+)" Added" ]]; then
				removed_vars=${BASH_REMATCH[1]} changed_vars=${BASH_REMATCH[2]} added_vars=${BASH_REMATCH[3]}
			fi

			if ((changed_leaf_types != 0)); then
				if ((new_so_maj == old_so_maj)); then
					abidiff_output=1
					touch $fail_file
					echo "Please update the major SO version for $so_file. A header accessible type has been modified since last release."
				fi
				found_abi_change=true
			fi

			if ((removed_functions != 0)) || ((removed_vars != 0)); then
				if ((new_so_maj == old_so_maj)); then
					abidiff_output=1
					touch $fail_file
					echo "Please update the major SO version for $so_file. API functions or variables have been removed since last release."
				fi
				found_abi_change=true
			fi

			if ((changed_functions != 0)) || ((changed_vars != 0)); then
				if ((new_so_maj == old_so_maj)); then
					abidiff_output=1
					touch $fail_file
					echo "Please update the major SO version for $so_file. API functions or variables have been changed since last release."
				fi
				found_abi_change=true
			fi

			if ((added_functions != 0)) || ((added_vars != 0)); then
				if ((new_so_min == old_so_min && new_so_maj == old_so_maj)) && ! $found_abi_change; then
					abidiff_output=1
					touch $fail_file
					echo "Please update the minor SO version for $so_file. API functions or variables have been added since last release."
				fi
				found_abi_change=true
			fi

			if [[ $so_name_changed == yes ]]; then
				# After 23.01 LTS all SO major versions were intentionally increased. This check can be removed after SPDK 23.05 release.
				if [[ "$release" == "v23.01.x" ]]; then
					found_abi_change=true
				fi
				if ! $found_abi_change; then
					echo "SO name for $so_file changed without a change to abi. please revert that change."
					touch $fail_file
				fi

				if ((new_so_maj != old_so_maj && new_so_min != 0)); then
					echo "SO major version for $so_file was bumped. Please reset the minor version to 0."
					touch $fail_file
				fi

				if ((new_so_min > old_so_min + 1)); then
					echo "SO minor version for $so_file was incremented more than once. Please revert minor version to $((old_so_min + 1))."
					touch $fail_file
				fi

				if ((new_so_maj > old_so_maj + 1)); then
					echo "SO major version for $so_file was incremented more than once. Please revert major version to $((old_so_maj + 1))."
					touch $fail_file
				fi
			fi

			if ((abidiff_output == 1)); then
				"${cmd_args[@]}" --impacted-interfaces || :
			fi
		fi
		processed_so=$((processed_so + 1))
	done
	rm -f $suppression_file
	echo "Processed $processed_so objects."
	rm -rf "$source_abi_dir"
}

function get_lib_shortname() {
	local lib=${1##*/}
	echo "${lib//@(libspdk_|.so)/}"
}

function import_libs_deps_mk() {
	local var_mk val_mk dep_mk fun_mk
	while read -r var_mk _ val_mk; do
		if [[ $var_mk == "#"* || ! $var_mk =~ (DEPDIRS-|_DEPS|_LIBS) ]]; then
			continue
		fi
		var_mk=${var_mk#*-}
		for dep_mk in $val_mk; do
			fun_mk=${dep_mk//@('$('|')')/}
			if [[ $fun_mk != "$dep_mk" ]]; then
				eval "${fun_mk}() { echo \$$fun_mk ; }"
			# Ignore any event_* dependencies. Those are based on the subsystem configuration and not readelf.
			elif ((IGNORED_LIBS["$dep_mk"] == 1)) || [[ $dep_mk =~ event_ ]]; then
				continue
			fi
			eval "$var_mk=\${$var_mk:+\$$var_mk }$dep_mk"
		done
	done < "$libdeps_file"
}

function confirm_deps() {
	local lib=$1 deplib lib_shortname

	lib_shortname=$(get_lib_shortname "$lib")
	lib_make_deps=(${!lib_shortname})

	symbols=($(readelf -s --wide "$lib" | grep -E "NOTYPE.*GLOBAL.*UND" | awk '{print $8}' | sort -u))
	symbols_regx=$(
		IFS="|"
		echo "(${symbols[*]})"
	)

	if ((${#symbols[@]} > 0)); then
		for deplib in "$libdir/"libspdk_!("$lib_shortname").so; do
			readelf -s --wide "$deplib" | grep -m1 -qE "DEFAULT\s+[0-9]+\s$symbols_regx$" || continue
			found_symbol_lib=$(get_lib_shortname "$deplib")
			# Ignore the env_dpdk readelf dependency. We don't want people explicitly linking against it.
			if [[ $found_symbol_lib != *env_dpdk* ]]; then
				dep_names+=("$found_symbol_lib")
			fi
		done
	fi

	diff=$(echo "${dep_names[@]}" "${lib_make_deps[@]}" | tr ' ' '\n' | sort | uniq -u)
	if [ "$diff" != "" ]; then
		touch $fail_file
		echo "there was a dependency mismatch in the library $lib_shortname"
		echo "The makefile (spdk.lib_deps.mk) lists: '${lib_make_deps[*]}'"
		echo "readelf outputs   : '${dep_names[*]}'"
		echo "---------------------------------------------------------------------"
	fi
}

function confirm_makefile_deps() {
	echo "---------------------------------------------------------------------"
	# Exclude libspdk_env_dpdk.so from the library list. We don't link against this one so that
	# users can define their own environment abstraction. However we do want to still check it
	# for dependencies to avoid printing out a bunch of confusing symbols under the missing
	# symbols section.
	SPDK_LIBS=("$libdir/"libspdk_!(env_dpdk).so)

	declare -A IGNORED_LIBS=()
	if grep -q 'CONFIG_RDMA?=n' $rootdir/mk/config.mk; then
		IGNORED_LIBS["rdma"]=1
	fi

	(
		import_libs_deps_mk
		for lib in "${SPDK_LIBS[@]}"; do confirm_deps "$lib" & done
		wait
	)
}

run_test "check_header_filenames" check_header_filenames

config_params=$(get_config_params)
if [ "$SPDK_TEST_OCF" -eq 1 ]; then
	config_params="$config_params --with-ocf=$rootdir/ocf.a"
fi

if [[ -f $rootdir/mk/config.mk ]]; then
	$MAKE $MAKEFLAGS clean
fi

$rootdir/configure $config_params --with-shared
# By setting SPDK_NO_LIB_DEPS=1, we ensure that we won't create any link dependencies.
# Then we can be sure we get a valid accounting of the symbol dependencies we have.
SPDK_NO_LIB_DEPS=1 $MAKE $MAKEFLAGS

xtrace_disable

fail_file=$output_dir/check_so_deps_fail

rm -f $fail_file

run_test "confirm_abi_deps" confirm_abi_deps

run_test "confirm_makefile_deps" confirm_makefile_deps

$MAKE $MAKEFLAGS clean

if [ -f $fail_file ]; then
	rm -f $fail_file
	echo "shared object test failed"
	exit 1
fi

xtrace_restore
