#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2019 Intel Corporation
#  All rights reserved.
#
shopt -s extglob

if [ "$(uname -s)" = "FreeBSD" ]; then
	echo "Not testing for shared object dependencies on FreeBSD."
	exit 1
fi

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)

function usage() {
	script_name="$(basename $0)"
	echo "Usage: $script_name"
	echo "    -c, --config-file     Rebuilds SPDK according to config file from autotest"
	echo "    -a, --spdk-abi-path   Use spdk-abi from specified path, otherwise"
	echo "                          latest version is pulled and deleted after test"
	echo "        --release=RELEASE Compare ABI against RELEASE"
	echo "    -h, --help            Print this help"
	echo "Example:"
	echo "$script_name -c ./autotest.config -a /path/to/spdk-abi"
}

# Parse input arguments #
while getopts 'hc:a:-:' optchar; do
	case "$optchar" in
		-)
			case "$OPTARG" in
				help)
					usage
					exit 0
					;;
				config-file=*)
					config_file="$(readlink -f ${OPTARG#*=})"
					;;
				spdk-abi-path=*)
					user_abi_dir="$(readlink -f ${OPTARG#*=})"
					;;
				release=*)
					RELEASE="${OPTARG#*=}"
					;;
				*) exit 1 ;;
			esac
			;;
		h)
			usage
			exit 0
			;;
		c) config_file="$(readlink -f ${OPTARG#*=})" ;;
		a) user_abi_dir="$(readlink -f ${OPTARG#*=})" ;;
		*) exit 1 ;;
	esac
done

source "$rootdir/test/common/autotest_common.sh"

if [[ -e $config_file ]]; then
	source "$config_file"
fi

source_abi_dir="${user_abi_dir:-"$testdir/abi"}"
libdir="$rootdir/build/lib"
libdeps_file="$rootdir/mk/spdk.lib_deps.mk"

function check_header_filenames() {
	local dups_found=0

	include_headers=$(git ls-files -- $rootdir/include/spdk $rootdir/include/spdk_internal | xargs -n 1 basename)
	dups=
	for file in $include_headers; do
		if [[ $(git ls-files "$rootdir/lib/**/$file" "$rootdir/module/**/$file" --error-unmatch 2> /dev/null) ]]; then
			dups+=" $file"
			dups_found=1
		fi
	done

	if ((dups_found == 1)); then
		echo "Private header file(s) found with same name as public header file."
		echo "This is not allowed since it can confuse abidiff when determining if"
		echo "data structure changes impact ABI."
		echo $dups
		return 1
	fi
}

function get_release() {
	local tag version major minor patch suffix

	if [[ -n "$1" ]]; then
		version="$1"
	else
		IFS='.-' read -r major minor patch suffix < "$rootdir/VERSION"
		version="v$major.$minor"
		((patch > 0)) && version+=".$patch"
		version+=${suffix:+-$suffix}
		# When tag does not already exist, search for last tag on current branch
		if ! git show-ref --tags "$version" --quiet; then
			unset version
		fi
	fi

	tag=$(git describe --tags --abbrev=0 --exclude=LTS --exclude="*-pre" $version)
	echo "${tag:0:6}"
}

function confirm_abi_deps() {
	local processed_so=0
	local abi_test_failed=false
	local abidiff_output
	local release
	local suppression_file="$testdir/abigail_suppressions.ini"

	release="${RELEASE:-$(get_release)}.x"

	if [[ ! -d $source_abi_dir ]]; then
		mkdir -p $source_abi_dir
		echo "spdk-abi has not been found at $source_abi_dir, cloning"
		git clone "https://github.com/spdk/spdk-abi.git" "$source_abi_dir"
	fi

	if [[ ! -d "$source_abi_dir/$release" ]]; then
		echo "Release (${release%.*}) does not exist in spdk-abi repository"
		return 1
	fi

	echo "* Running ${FUNCNAME[0]} against the ${release%.*} release" >&2

	if ! hash abidiff; then
		echo "Unable to check ABI compatibility. Please install abidiff."
		return 1
	fi

	# Type suppression should be used for deliberate change in the structure,
	# that do not affect the ABI compatibility.
	# One example is utilizing a previously reserved field in the structure.
	# Suppression file should contain at the very least the name of type
	# and point to major SO_VER of the library it applies to.
	#
	# To avoid ignoring an actual changes in ABI for a particular SO_VER,
	# the suppression should be as specific as possible to the change.
	# has_data_member_* conditions can be used to narrow down the
	# name of previously existing field, or specify the offset.
	# Please refer to libabigail for details.
	#
	# Example format:
	# [suppress_type]
	#	label = Added interrupt_mode field
	#	name = spdk_app_opts
	#	soname_regexp = ^libspdk_event\\.so\\.12\\.*$
	cat <<- EOF > ${suppression_file}
		[suppress_type]
			label = Removed spdk_nvme_accel_fn_table.submit_accel_crc32c field
			name = spdk_nvme_accel_fn_table
			soname_regexp = ^libspdk_nvme\\.so\\.14\\.*$
			has_data_member_regexp = ^submit_accel_crc32c$
			has_data_member_inserted_between = {64, 128}
		[suppress_type]
			label = Added num_trace_threads field using reserved space with padding reorganization
			name = spdk_app_opts
			soname_regexp = ^libspdk_event\\.so\\.15\\.*$
			has_data_member_regexp = ^(reserved191|reserved187|num_trace_threads)$
		[suppress_type]
			label = Added new registers from NVMe 2.0 using reserved space
			soname_regexp = ^libspdk_nvme\\.so\\.16\\.*$|^libspdk_nvmf\\.so\\.21\\.*$
			name = spdk_nvme_registers
			has_data_member_inserted_between = {offset_after(cmbsts), offset_of(pmrcap)}
		[suppress_type]
			label = Added new bits from NVMe 2.0 to CAP register using reserved space
			soname_regexp = ^libspdk_nvme\\.so\\.16\\.*$|^libspdk_nvmf\\.so\\.21\\.*$
			name = spdk_nvme_cap_register
			has_data_member_inserted_between = {offset_after(cmbs), offset_of(reserved3)}
		[suppress_type]
			label = Added new CRIME bit to CC register from NVMe 2.0 using reserved space
			soname_regexp = ^libspdk_nvme\\.so\\.16\\.*$|^libspdk_nvmf\\.so\\.21\\.*$
			name_regexp = spdk_nvme_cc_register
			has_data_member_inserted_at = 24
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
					abi_test_failed=true
					echo "Please update the major SO version for $so_file. A header accessible type has been modified since last release."
				fi
				found_abi_change=true
			fi

			if ((removed_functions != 0)) || ((removed_vars != 0)); then
				if ((new_so_maj == old_so_maj)); then
					abidiff_output=1
					abi_test_failed=true
					echo "Please update the major SO version for $so_file. API functions or variables have been removed since last release."
				fi
				found_abi_change=true
			fi

			if ((changed_functions != 0)) || ((changed_vars != 0)); then
				if ((new_so_maj == old_so_maj)); then
					abidiff_output=1
					abi_test_failed=true
					echo "Please update the major SO version for $so_file. API functions or variables have been changed since last release."
				fi
				found_abi_change=true
			fi

			if ((added_functions != 0)) || ((added_vars != 0)); then
				if ((new_so_min == old_so_min && new_so_maj == old_so_maj)) && ! $found_abi_change; then
					abidiff_output=1
					abi_test_failed=true
					echo "Please update the minor SO version for $so_file. API functions or variables have been added since last release."
				fi
				found_abi_change=true
			fi

			if [[ $so_name_changed == yes ]]; then
				# All SO major versions are intentionally increased after LTS to allow SO minor changes during the supported period.
				if [[ "$release" == "$(get_release LTS).x" ]]; then
					found_abi_change=true
				fi
				if ! $found_abi_change; then
					echo "SO name for $so_file changed without a change to abi. please revert that change."
					abi_test_failed=true
				fi

				if ((new_so_maj != old_so_maj && new_so_min != 0)); then
					echo "SO major version for $so_file was bumped. Please reset the minor version to 0."
					abi_test_failed=true
				fi

				if ((new_so_min > old_so_min + 1)); then
					echo "SO minor version for $so_file was incremented more than once. Please revert minor version to $((old_so_min + 1))."
					abi_test_failed=true
				fi

				if ((new_so_maj > old_so_maj + 1)); then
					echo "SO major version for $so_file was incremented more than once. Please revert major version to $((old_so_maj + 1))."
					abi_test_failed=true
				fi
			fi

			if ((abidiff_output == 1)); then
				"${cmd_args[@]}" --impacted-interfaces || :
			fi
		fi
		processed_so=$((processed_so + 1))
	done
	rm -f $suppression_file
	if [[ "$processed_so" -eq 0 ]]; then
		echo "No shared objects were processed."
		return 1
	fi
	echo "Processed $processed_so objects."
	if [[ -z $user_abi_dir ]]; then
		rm -rf "$source_abi_dir"
	fi
	if $abi_test_failed; then
		echo "ERROR: ABI test failed"
		exit 1
	fi
}

list_deps_mk() {
	local tab=$'\t'

	make -f - <<- MAKE
		SPDK_ROOT_DIR := $rootdir
		include \$(SPDK_ROOT_DIR)/mk/spdk.common.mk
		include \$(SPDK_ROOT_DIR)/mk/spdk.lib_deps.mk

		all: \$(filter DEPDIRS-%,\$(.VARIABLES))

		# Ignore any event_* dependencies. Those are based on the subsystem
		# configuration and not readelf
		DEPDIRS-%:
			$tab@echo "\$(@:DEPDIRS-%=%)=\"\$(filter-out event_%,\$(\$@))\""
	MAKE
}

function get_lib_shortname() {
	local lib=${1##*/}
	echo "${lib//@(libspdk_|.so)/}"
}

# shellcheck disable=SC2005
sort_libs() { echo $(printf "%s\n" "$@" | sort); }

function confirm_deps() {
	local lib=$1 deplib lib_shortname symbols dep_names lib_make_deps found_symbol_lib

	lib_shortname=$(get_lib_shortname "$lib")
	lib_make_deps=(${!lib_shortname})

	symbols=($(readelf -s --wide "$lib" | awk '$7 == "UND" {print $8}' | sort -u))
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
		cat <<- MSG
			There was a dependency mismatch in the library $lib_shortname
			The makefile (spdk.lib_deps.mk) lists: '$(sort_libs "${lib_make_deps[@]}")'
			readelf outputs:                       '$(sort_libs "${dep_names[@]}")'
		MSG
	fi
}

function confirm_makefile_deps() {
	echo "---------------------------------------------------------------------"
	# Exclude libspdk_env_dpdk.so from the library list. We don't link against this one so that
	# users can define their own environment abstraction. However we do want to still check it
	# for dependencies to avoid printing out a bunch of confusing symbols under the missing
	# symbols section.
	SPDK_LIBS=("$libdir/"libspdk_!(env_dpdk).so)
	fail_file="$testdir/check_so_deps_fail"

	rm -f $fail_file

	declare -A IGNORED_LIBS=()
	if [[ $CONFIG_RDMA == n ]]; then
		IGNORED_LIBS["rdma"]=1
	fi

	(
		source <(list_deps_mk)
		for lib in "${SPDK_LIBS[@]}"; do confirm_deps "$lib" & done
		wait
	)

	if [ -f $fail_file ]; then
		rm -f $fail_file
		echo "ERROR: Makefile deps test failed"
		exit 1
	fi
}

if [[ -e $config_file ]]; then
	config_params=$(get_config_params)
	if [[ "$SPDK_TEST_OCF" -eq 1 ]]; then
		config_params="$config_params --with-ocf=$rootdir/ocf.a"
	fi

	if [[ -f $rootdir/mk/config.mk ]]; then
		$MAKE $MAKEFLAGS clean
	fi

	$rootdir/configure $config_params --with-shared
	$MAKE $MAKEFLAGS
fi

xtrace_disable

run_test "check_header_filenames" check_header_filenames
run_test "confirm_abi_deps" confirm_abi_deps
run_test "confirm_makefile_deps" confirm_makefile_deps

if [[ -e $config_file ]]; then
	$MAKE $MAKEFLAGS clean
fi

xtrace_restore
